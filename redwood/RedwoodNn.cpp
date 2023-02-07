#include <array>
#include <iostream>

#include "../include/PointCloud.hpp"
#include "../include/Redwood.hpp"
// #include "Kernel.hpp"
#include "NnBuffer.hpp"
#include "accelerator/Core.hpp"
#include "accelerator/Kernels.hpp"
#include "rt/Runtime.hpp"

namespace redwood {

// ------------------- Constants -------------------

constexpr auto kNumStreams = 2;
int stored_leaf_size;
int stored_num_batches;
int stored_num_threads;

template <typename QueryT, typename ResultT>
struct ReducerHandler {
  void Init(const int batch_num) {
    for (int i = 0; i < kNumStreams; ++i) {
      nn_buffers[i].Allocate(batch_num);

      accelerator::AttachStreamMem(i, nn_buffers[i].query_point.data());
      accelerator::AttachStreamMem(i, nn_buffers[i].query_idx.data());
      accelerator::AttachStreamMem(i, nn_buffers[i].leaf_idx.data());
    }
  };

  std::array<NnBuffer<QueryT>, kNumStreams> nn_buffers;
  std::vector<NnResult<ResultT>> nn_results;

  NnBuffer<QueryT>& CurrentBuffer() { return nn_buffers[cur_collecting]; }
  NnResult<ResultT>& CurrentResult() { return nn_results[cur_collecting]; }

  int cur_collecting = 0;
};

// ------------------- Global Shared  -------------------

const Point4F* host_query_point_ref;
const Point4F* usm_leaf_node_table_ref;
ReducerHandler<Point4F, float>* rhs;

// ------------------- Public APIs  -------------------

void InitReducer(const int num_threads, const int leaf_size,
                 const int batch_num, const int batch_size) {
  stored_leaf_size = leaf_size;
  stored_num_threads = num_threads;
  stored_num_batches = batch_num;

  accelerator::Initialization();

  rhs = new ReducerHandler<Point4F, float>[num_threads];
  for (int i = 0; i < num_threads; ++i) {
    rhs[i].Init(batch_num);
  }
}

void SetQueryPoints(const int tid, const void* query_points,
                    const int num_query) {
  host_query_point_ref = static_cast<const Point4F*>(query_points);

  rhs[tid].nn_results.reserve(kNumStreams);
  rhs[tid].nn_results.emplace_back(num_query);
  rhs[tid].nn_results.emplace_back(num_query);

  accelerator::AttachStreamMem(0, rhs[tid].nn_results[0].results.data());
  accelerator::AttachStreamMem(1, rhs[tid].nn_results[1].results.data());
}

void SetNodeTables(const void* usm_leaf_node_table, const int num_leaf_nodes) {
  usm_leaf_node_table_ref = static_cast<const Point4F*>(usm_leaf_node_table);
}

void ReduceLeafNode(const int tid, const int node_idx, const int query_idx) {
  rhs[tid].CurrentBuffer().Push(host_query_point_ref[query_idx], query_idx,
                                node_idx);
}

void GetReductionResult(const int tid, const int query_idx, void* result) {
  auto addr = static_cast<float**>(result);
  *addr = &rhs[tid].CurrentResult().results[query_idx];
}

void EndReducer() { delete[] rhs; }

// ------------------- Developer APIs  -------------------

void rt::ExecuteCurrentBufferAsync(int tid, int num_batch_collected) {
  const auto& cb = rhs[tid].CurrentBuffer();
  const auto current_stream = rhs[tid].cur_collecting;

  // Application specific
  // TODO: Make it decoupled
  accelerator::LaunchNnKernel(
      cb.query_point.data(), usm_leaf_node_table_ref, cb.query_idx.data(),
      cb.leaf_idx.data(), rhs[tid].CurrentResult().results.data(),
      num_batch_collected, stored_leaf_size, current_stream);

  const auto next_stream = (kNumStreams - 1) - current_stream;
  accelerator::DeviceStreamSynchronize(next_stream);

  rhs[tid].nn_buffers[next_stream].Clear();
  rhs[tid].cur_collecting = next_stream;
}

}  // namespace redwood