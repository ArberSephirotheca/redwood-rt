#include <array>
#include <iostream>

#include "BhBuffer.hpp"
#include "PointCloud.hpp"
#include "Redwood.hpp"
#include "UsmAlloc.hpp"
#include "accelerator/Core.hpp"
#include "accelerator/Kernels.hpp"
#include "rt/Runtime.hpp"

namespace redwood {

// ------------------- Constants -------------------

constexpr auto kNumStreams = 2;
int stored_leaf_size;
int stored_num_batches;
int stored_batch_size;
int stored_num_threads;

template <typename DataT, typename QueryT, typename ResultT>
struct ReducerHandler {
  using MyBhBuffer = BhBuffer<DataT, QueryT, ResultT>;

  void Init(const int batch_num) {
    for (int i = 0; i < kNumStreams; ++i) {
      bh_buffers[i].Allocate(batch_num);

      accelerator::AttachStreamMem(i, bh_buffers[i].leaf_nodes.data());
      accelerator::AttachStreamMem(i, bh_buffers[i].branch_data.data());
    }
  };

  std::array<MyBhBuffer, kNumStreams> bh_buffers;
  std::array<UsmVector<ResultT>, kNumStreams> bh_results;

  MyBhBuffer& CurrentBuffer() { return bh_buffers[cur_collecting]; }
  ResultT* CurrentResultData() { return bh_results[cur_collecting].data(); }

  int cur_collecting = 0;
};

// ------------------- Global Shared  -------------------

const Point3F* host_query_point_ref;
const Point4F* usm_leaf_node_table_ref;

ReducerHandler<Point4F, Point3F, Point3F>* rhs;

// ------------------- Public APIs  -------------------

void InitReducer(const int num_threads, const int leaf_size,
                 const int batch_num, const int batch_size) {
  stored_leaf_size = leaf_size;
  stored_num_threads = num_threads;
  stored_num_batches = batch_num;

  accelerator::Initialization();

  rhs = new ReducerHandler<Point4F, Point3F, Point3F>[num_threads];
  for (int i = 0; i < num_threads; ++i) {
    rhs[i].Init(batch_num);
  }
}

void SetQueryPoints(const int tid, const void* query_points,
                    const int num_query) {
  host_query_point_ref = static_cast<const Point3F*>(query_points);

  rhs[tid].bh_results[0].resize(num_query);
  rhs[tid].bh_results[1].resize(num_query);

  accelerator::AttachStreamMem(0, rhs[tid].bh_results[0].data());
  accelerator::AttachStreamMem(1, rhs[tid].bh_results[1].data());
}

void SetNodeTables(const void* usm_leaf_node_table, const int num_leaf_nodes) {
  usm_leaf_node_table_ref = static_cast<const Point4F*>(usm_leaf_node_table);
}

void StartQuery(const int tid, const int query_idx) {
  rhs[tid].CurrentBuffer().SetTask(host_query_point_ref[query_idx], query_idx);
}

void ReduceLeafNode(const int tid, const int node_idx, const int query_idx) {
  rhs[tid].CurrentBuffer().PushLeaf(node_idx);
}

void ReduceBranchNode(int tid, const void* node_element, int query_idx) {
  rhs[tid].CurrentBuffer().PushBranch(
      *static_cast<const Point4F*>(node_element));
}

void GetReductionResult(const int tid, const int query_idx, void* result) {
  auto addr = static_cast<Point3F**>(result);
  *addr = &rhs[tid].CurrentResultData()[query_idx];
}

void EndReducer() { delete[] rhs; }

// ------------------- Developer APIs  -------------------

void rt::ExecuteCurrentBufferAsync(int tid, int num_batch_collected) {
  const auto& cb = rhs[tid].CurrentBuffer();
  const auto current_stream = rhs[tid].cur_collecting;

  // The current implementation process on query only
  accelerator::LaunchBhKernel(
      cb.my_query, usm_leaf_node_table_ref, cb.LeafNodeData(),
      cb.NumLeafsCollected(), cb.BranchNodeData(), cb.NumBranchCollected(),
      rhs[tid].CurrentResultData(), stored_leaf_size, current_stream);

  std::cout << "DEBUG: " << *rhs[tid].CurrentResultData() << std::endl;

  const auto next_stream = (kNumStreams - 1) - current_stream;
  accelerator::DeviceStreamSynchronize(next_stream);

  // TODO: Add another internal call for 'OnProcessFinish'

  rhs[tid].bh_buffers[next_stream].Clear();
  rhs[tid].cur_collecting = next_stream;
}

}  // namespace redwood