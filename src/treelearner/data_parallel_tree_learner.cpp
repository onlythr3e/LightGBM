#include "parallel_tree_learner.h"

#include <cstring>

#include <tuple>
#include <vector>

namespace LightGBM {

DataParallelTreeLearner::DataParallelTreeLearner(const TreeConfig* tree_config)
  :SerialTreeLearner(tree_config) {
}

DataParallelTreeLearner::~DataParallelTreeLearner() {

}

void DataParallelTreeLearner::Init(const Dataset* train_data) {
  // initialize SerialTreeLearner
  SerialTreeLearner::Init(train_data);
  // Get local rank and global machine size
  rank_ = Network::rank();
  num_machines_ = Network::num_machines();
  // allocate buffer for communication
  size_t buffer_size = train_data_->NumTotalBin() * sizeof(HistogramBinEntry);

  input_buffer_.resize(buffer_size);
  output_buffer_.resize(buffer_size);

  is_feature_aggregated_.resize(num_features_);

  block_start_.resize(num_machines_);
  block_len_.resize(num_machines_);

  buffer_write_start_pos_.resize(num_features_);
  buffer_read_start_pos_.resize(num_features_);
  global_data_count_in_leaf_.resize(tree_config_->num_leaves);
}

void DataParallelTreeLearner::ResetConfig(const TreeConfig* tree_config) {
  SerialTreeLearner::ResetConfig(tree_config);
  global_data_count_in_leaf_.resize(tree_config_->num_leaves);
}

void DataParallelTreeLearner::BeforeTrain() {
  SerialTreeLearner::BeforeTrain();
  // generate feature partition for current tree
  std::vector<std::vector<int>> feature_distribution(num_machines_, std::vector<int>());
  std::vector<int> num_bins_distributed(num_machines_, 0);
  for (int i = 0; i < train_data_->num_total_features(); ++i) {
    int inner_feature_index = train_data_->InnerFeatureIndex(i);
    if (inner_feature_index == -1) { continue; }
    if (is_feature_used_[inner_feature_index]) {
      int cur_min_machine = static_cast<int>(ArrayArgs<int>::ArgMin(num_bins_distributed));
      feature_distribution[cur_min_machine].push_back(inner_feature_index);
      auto num_bin = train_data_->FeatureNumBin(inner_feature_index);
      if (train_data_->FeatureBinMapper(inner_feature_index)->GetDefaultBin() == 0) {
        num_bin -= 1;
      }
      num_bins_distributed[cur_min_machine] += num_bin;
    }
    is_feature_aggregated_[inner_feature_index] = false;
  }
  // get local used feature
  for (auto fid : feature_distribution[rank_]) {
    is_feature_aggregated_[fid] = true;
  }

  // get block start and block len for reduce scatter
  reduce_scatter_size_ = 0;
  for (int i = 0; i < num_machines_; ++i) {
    block_len_[i] = 0;
    for (auto fid : feature_distribution[i]) {
      auto num_bin = train_data_->FeatureNumBin(fid);
      if (train_data_->FeatureBinMapper(fid)->GetDefaultBin() == 0) {
        num_bin -= 1;
      }
      block_len_[i] += num_bin * sizeof(HistogramBinEntry);
    }
    reduce_scatter_size_ += block_len_[i];
  }

  block_start_[0] = 0;
  for (int i = 1; i < num_machines_; ++i) {
    block_start_[i] = block_start_[i - 1] + block_len_[i - 1];
  }

  // get buffer_write_start_pos_
  int bin_size = 0;
  for (int i = 0; i < num_machines_; ++i) {
    for (auto fid : feature_distribution[i]) {
      buffer_write_start_pos_[fid] = bin_size;
      auto num_bin = train_data_->FeatureNumBin(fid);
      if (train_data_->FeatureBinMapper(fid)->GetDefaultBin() == 0) {
        num_bin -= 1;
      }
      bin_size += num_bin * sizeof(HistogramBinEntry);
    }
  }

  // get buffer_read_start_pos_
  bin_size = 0;
  for (auto fid : feature_distribution[rank_]) {
    buffer_read_start_pos_[fid] = bin_size;
    auto num_bin = train_data_->FeatureNumBin(fid);
    if (train_data_->FeatureBinMapper(fid)->GetDefaultBin() == 0) {
      num_bin -= 1;
    }
    bin_size += num_bin * sizeof(HistogramBinEntry);
  }

  // sync global data sumup info
  std::tuple<data_size_t, double, double> data(smaller_leaf_splits_->num_data_in_leaf(),
    smaller_leaf_splits_->sum_gradients(), smaller_leaf_splits_->sum_hessians());
  int size = sizeof(data);
  std::memcpy(input_buffer_.data(), &data, size);
  // global sumup reduce
  Network::Allreduce(input_buffer_.data(), size, size, output_buffer_.data(), [](const char *src, char *dst, int len) {
    int used_size = 0;
    int type_size = sizeof(std::tuple<data_size_t, double, double>);
    const std::tuple<data_size_t, double, double> *p1;
    std::tuple<data_size_t, double, double> *p2;
    while (used_size < len) {
      p1 = reinterpret_cast<const std::tuple<data_size_t, double, double> *>(src);
      p2 = reinterpret_cast<std::tuple<data_size_t, double, double> *>(dst);
      std::get<0>(*p2) = std::get<0>(*p2) + std::get<0>(*p1);
      std::get<1>(*p2) = std::get<1>(*p2) + std::get<1>(*p1);
      std::get<2>(*p2) = std::get<2>(*p2) + std::get<2>(*p1);
      src += type_size;
      dst += type_size;
      used_size += type_size;
    }
  });
  // copy back
  std::memcpy(&data, output_buffer_.data(), size);
  // set global sumup info
  smaller_leaf_splits_->Init(std::get<1>(data), std::get<2>(data));
  // init global data count in leaf
  global_data_count_in_leaf_[0] = std::get<0>(data);
}

void DataParallelTreeLearner::FindBestThresholds() {
  train_data_->ConstructHistograms(is_feature_used_,
    smaller_leaf_splits_->data_indices(), smaller_leaf_splits_->num_data_in_leaf(),
    smaller_leaf_splits_->LeafIndex(),
    ordered_bins_, gradients_, hessians_,
    ordered_gradients_.data(), ordered_hessians_.data(),
    smaller_leaf_histogram_array_[0].RawData() - 1);
  // construct local histograms
#pragma omp parallel for schedule(static)
  for (int feature_index = 0; feature_index < num_features_; ++feature_index) {
    if ((!is_feature_used_.empty() && is_feature_used_[feature_index] == false)) continue;
    // copy to buffer
    std::memcpy(input_buffer_.data() + buffer_write_start_pos_[feature_index],
      smaller_leaf_histogram_array_[feature_index].RawData(),
      smaller_leaf_histogram_array_[feature_index].SizeOfHistgram());
  }
  // Reduce scatter for histogram
  Network::ReduceScatter(input_buffer_.data(), reduce_scatter_size_, block_start_.data(),
    block_len_.data(), output_buffer_.data(), &HistogramBinEntry::SumReducer);

  std::vector<SplitInfo> smaller_best(num_threads_, SplitInfo());
  std::vector<SplitInfo> larger_best(num_threads_, SplitInfo());
#pragma omp parallel for schedule(static)
  for (int feature_index = 0; feature_index < num_features_; ++feature_index) {
    if (!is_feature_aggregated_[feature_index]) continue;
    const int tid = omp_get_thread_num();
    // restore global histograms from buffer
    smaller_leaf_histogram_array_[feature_index].FromMemory(
      output_buffer_.data() + buffer_read_start_pos_[feature_index]);

    train_data_->FixHistogram(feature_index,
      smaller_leaf_splits_->sum_gradients(), smaller_leaf_splits_->sum_hessians(),
      GetGlobalDataCountInLeaf(smaller_leaf_splits_->LeafIndex()),
      smaller_leaf_histogram_array_[feature_index].RawData());
    SplitInfo smaller_split;
    // find best threshold for smaller child
    smaller_leaf_histogram_array_[feature_index].FindBestThreshold(
      smaller_leaf_splits_->sum_gradients(),
      smaller_leaf_splits_->sum_hessians(),
      GetGlobalDataCountInLeaf(smaller_leaf_splits_->LeafIndex()),
      &smaller_split);
    if (smaller_split.gain > smaller_best[tid].gain) {
      smaller_best[tid] = smaller_split;
      smaller_best[tid].feature = train_data_->RealFeatureIndex(feature_index);
    }

    // only root leaf
    if (larger_leaf_splits_ == nullptr || larger_leaf_splits_->LeafIndex() < 0) continue;

    // construct histgroms for large leaf, we init larger leaf as the parent, so we can just subtract the smaller leaf's histograms
    larger_leaf_histogram_array_[feature_index].Subtract(
      smaller_leaf_histogram_array_[feature_index]);
    SplitInfo larger_split;
    // find best threshold for larger child
    larger_leaf_histogram_array_[feature_index].FindBestThreshold(
      larger_leaf_splits_->sum_gradients(),
      larger_leaf_splits_->sum_hessians(),
      GetGlobalDataCountInLeaf(larger_leaf_splits_->LeafIndex()),
      &larger_split);
    if (larger_split.gain > larger_best[tid].gain) {
      larger_best[tid] = larger_split;
      larger_best[tid].feature = train_data_->RealFeatureIndex(feature_index);
    }
  }
  auto smaller_best_idx = ArrayArgs<SplitInfo>::ArgMax(smaller_best);
  int leaf = smaller_leaf_splits_->LeafIndex();
  best_split_per_leaf_[leaf] = smaller_best[smaller_best_idx];


  if (larger_leaf_splits_ == nullptr || larger_leaf_splits_->LeafIndex() < 0) { return; }

  leaf = larger_leaf_splits_->LeafIndex();
  auto larger_best_idx = ArrayArgs<SplitInfo>::ArgMax(larger_best);
  best_split_per_leaf_[leaf] = larger_best[larger_best_idx];

}

void DataParallelTreeLearner::FindBestSplitsForLeaves() {
  SplitInfo smaller_best, larger_best;
  smaller_best = best_split_per_leaf_[smaller_leaf_splits_->LeafIndex()];
  // find local best split for larger leaf
  if (larger_leaf_splits_->LeafIndex() >= 0) {
    larger_best = best_split_per_leaf_[larger_leaf_splits_->LeafIndex()];
  }

  // sync global best info
  std::memcpy(input_buffer_.data(), &smaller_best, sizeof(SplitInfo));
  std::memcpy(input_buffer_.data() + sizeof(SplitInfo), &larger_best, sizeof(SplitInfo));

  Network::Allreduce(input_buffer_.data(), sizeof(SplitInfo) * 2, sizeof(SplitInfo),
    output_buffer_.data(), &SplitInfo::MaxReducer);

  std::memcpy(&smaller_best, output_buffer_.data(), sizeof(SplitInfo));
  std::memcpy(&larger_best, output_buffer_.data() + sizeof(SplitInfo), sizeof(SplitInfo));

  // set best split
  best_split_per_leaf_[smaller_leaf_splits_->LeafIndex()] = smaller_best;
  if (larger_leaf_splits_->LeafIndex() >= 0) {
    best_split_per_leaf_[larger_leaf_splits_->LeafIndex()] = larger_best;
  }
}

void DataParallelTreeLearner::Split(Tree* tree, int best_Leaf, int* left_leaf, int* right_leaf) {
  SerialTreeLearner::Split(tree, best_Leaf, left_leaf, right_leaf);
  const SplitInfo& best_split_info = best_split_per_leaf_[best_Leaf];
  // need update global number of data in leaf
  global_data_count_in_leaf_[*left_leaf] = best_split_info.left_count;
  global_data_count_in_leaf_[*right_leaf] = best_split_info.right_count;
}


}  // namespace LightGBM
