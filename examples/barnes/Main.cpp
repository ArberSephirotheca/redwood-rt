#include <omp.h>

#include <algorithm>
#include <array>
#include <numeric>
#include <queue>
#include <vector>

#include "../LoadFile.hpp"
#include "../Utils.hpp"
#include "../cxxopts.hpp"
#include "AppParams.hpp"
#include "Functors/DistanceMetrics.hpp"
#include "Octree.hpp"
#include "ReducerHandler.hpp"
#include "Redwood.hpp"

using Task = std::pair<int, Point4F>;

struct ExecutorStats {
  int leaf_node_reduced = 0;
  int branch_node_reduced = 0;
};

// Traverser class for BH Algorithm
class Executor {
  // Store some reference used
  const int my_tid_;
  const int my_stream_id_;
  ExecutorStats stats_;

  Point4F my_q_;

  // Used on the CPU side
  float host_result_;

 public:
  Executor(const int tid, const int stream_id)
      : my_tid_(tid), my_stream_id_(stream_id) {}

  void StartQuery(const Point4F q, const oct::Node<float>* root) {
    // Clear executor's data
    stats_.leaf_node_reduced = 0;
    stats_.branch_node_reduced = 0;

    // This is a host copy of q point, so some times the traversal doesn't have
    // to bother with accessing USM
    my_q_ = q;

    // Notify Reducer to
    // In case of FPGA, it will register the anchor point (q) into the registers
    rdc::SetQuery(my_tid_, my_stream_id_, my_q_);

    TraverseRecursive(root);
  }

  void StartQueryCpu(const Point4F q, const oct::Node<float>* root) {
    stats_.leaf_node_reduced = 0;
    stats_.branch_node_reduced = 0;
    my_q_ = q;
    host_result_ = 0.0f;
    TraverseRecursiveCpu(root);
  }

  _NODISCARD ExecutorStats GetStats() const { return stats_; }

  _NODISCARD float GetCpuResult() const { return host_result_; }

 private:
  _NODISCARD static float ComputeThetaValue(const oct::Node<float>* node,
                                            const Point4F& pos) {
    const auto com = node->CenterOfMass();
    auto norm_sqr = 1e-9f;

    // Use only the first three property (x, y, z) for this theta compuation
    for (int i = 0; i < 3; ++i) {
      const auto diff = com.data[i] - pos.data[i];
      norm_sqr += diff * diff;
    }

    const auto norm = sqrtf(norm_sqr);
    return node->bounding_box.dimension.data[0] / norm;
  }

  // Main Barnes-Hut Traversal Algorithm, annotated with Redwood APIs
  void TraverseRecursive(const oct::Node<float>* cur) {
    if (cur->IsLeaf()) {
      if (cur->bodies.empty()) return;

      // ------------------------------------------------------------
      rdc::ReduceLeafNode(my_tid_, my_stream_id_, cur->uid);
      // ------------------------------------------------------------

      ++stats_.leaf_node_reduced;
    } else if (const auto my_theta = ComputeThetaValue(cur, my_q_);
               my_theta < app_params.theta) {
      ++stats_.branch_node_reduced;

      // ------------------------------------------------------------
      rdc::ReduceBranchNode(my_tid_, my_stream_id_, cur->CenterOfMass());
      // ---------------------------------------------------------------

    } else
      for (const auto child : cur->children)
        if (child != nullptr) TraverseRecursive(child);
  }

  // CPU version
  void TraverseRecursiveCpu(const oct::Node<float>* cur) {
    constexpr dist::Gravity functor;

    if (cur->IsLeaf()) {
      if (cur->bodies.empty()) return;

      // ------------------------------------------------------------
      const auto leaf_addr = rdc::LntDataAddrAt(cur->uid);
      for (int i = 0; i < app_params.max_leaf_size; ++i) {
        host_result_ += functor(my_q_, leaf_addr[i]);
      }
      // ------------------------------------------------------------

      ++stats_.leaf_node_reduced;
    } else if (const auto my_theta = ComputeThetaValue(cur, my_q_);
               my_theta < app_params.theta) {
      ++stats_.branch_node_reduced;

      // ------------------------------------------------------------
      host_result_ += functor(my_q_, cur->CenterOfMass());
      // ---------------------------------------------------------------

    } else
      for (const auto child : cur->children)
        if (child != nullptr) TraverseRecursiveCpu(child);
  }
};

_NODISCARD inline Point4F RandPoint() {
  Point4F p;
  p.data[0] = MyRand(0, 1000);
  p.data[1] = MyRand(0, 1000);
  p.data[2] = MyRand(0, 1000);
  p.data[3] = MyRand(0, 1000);
  return p;
}

int main(int argc, char** argv) {
  cxxopts::Options options("Barnes-Hut (BH)", "Redwood BH demo implementation");

  // clang-format off
  options.add_options()
    ("f,file", "Input file name", cxxopts::value<std::string>())
    ("m,query", "Number of particles to query", cxxopts::value<int>()->default_value("1048576"))
    ("t,thread", "Number of threads", cxxopts::value<int>()->default_value("1"))
    ("theta", "Theta Value", cxxopts::value<float>()->default_value("0.2"))
    ("l,leaf", "Maximum leaf node size", cxxopts::value<int>()->default_value("32"))
    ("b,batch_size", "Batch size (GPU)", cxxopts::value<int>()->default_value("2048"))
    ("c,cpu", "Enable CPU baseline", cxxopts::value<bool>()->default_value("false"))
    ("h,help", "Print usage");
  // clang-format on

  options.parse_positional({"file", "query"});

  const auto result = options.parse(argc, argv);

  if (result.count("help")) {
    std::cout << options.help() << std::endl;
    exit(EXIT_SUCCESS);
  }

  if (!result.count("file")) {
    std::cerr << "requires an input file (\"../../data/2m_bh_uniform.dat\")\n";
    std::cout << options.help() << std::endl;
    exit(EXIT_FAILURE);
  }

  const auto data_file = result["file"].as<std::string>();
  app_params.m = result["query"].as<int>();
  app_params.num_threads = result["thread"].as<int>();
  app_params.theta = result["theta"].as<float>();
  app_params.max_leaf_size = result["leaf"].as<int>();
  app_params.batch_size = result["batch_size"].as<int>();
  app_params.cpu = result["cpu"].as<bool>();
  std::cout << app_params << std::endl;

  std::cout << "Loading Data..." << std::endl;

  const auto in_data = load_data_from_file<Point4F>(data_file);
  const auto n = in_data.size();

  // For each thread
  std::vector<std::queue<Task>> q_data(app_params.num_threads);
  const auto tasks_per_thread = app_params.m / app_params.num_threads;
  for (int tid = 0; tid < app_params.num_threads; ++tid) {
    for (int i = 0; i < tasks_per_thread; ++i) {
      q_data[tid].emplace(i, RandPoint());
    }
  }

  std::cout << "Building Tree..." << std::endl;

  const oct::BoundingBox<float> universe{
      Point3F{1000.0f, 1000.0f, 1000.0f},
      Point3F{500.0f, 500.0f, 500.0f},
  };
  const oct::OctreeParams<float> params{
      app_params.theta, static_cast<size_t>(app_params.max_leaf_size),
      universe};
  oct::Octree<float> tree(in_data.data(), static_cast<int>(n), params);
  tree.BuildTree();

  // Init
  rdc::Init(app_params.num_threads, app_params.batch_size);
  omp_set_num_threads(app_params.num_threads);

  const auto num_leaf_nodes = tree.GetStats().num_leaf_nodes;
  auto [lnt_addr, lnt_size_addr] =
      rdc::AllocateLnt(num_leaf_nodes, app_params.max_leaf_size);

  tree.LoadPayload(lnt_addr, lnt_size_addr);

  std::vector<float> final_results;
  final_results.resize(app_params.m);  // need to discard the first

  std::cout << "Starting Traversal... " << std::endl;

  TimeTask("Traversal", [&] {
    if (app_params.cpu) {
      // ------------------- CPU ------------------------------------
      std::vector<Executor> cpu_exe;

      for (int tid = 0; tid < app_params.num_threads; ++tid) {
        cpu_exe.emplace_back(tid, 0);
      }

      // Run
#pragma omp parallel for
      for (int tid = 0; tid < app_params.num_threads; ++tid) {
        while (!q_data[tid].empty()) {
          const auto [q_idx, q] = q_data[tid].front();
          cpu_exe[tid].StartQueryCpu(q, tree.GetRoot());
          final_results[q_idx] = cpu_exe[tid].GetCpuResult();
          q_data[tid].pop();
        }
      }

      // -------------------------------------------------------------
    } else {
      // ------------------- CUDA ------------------------------------

      // -------------------------------------------------------------
    }
  });

  // -------------------------------------------------------------

  for (int i = 0; i < 5; ++i) {
    const auto q = final_results[i];
    std::cout << i << ": " << q << std::endl;
  }

  rdc::Release();
  return EXIT_SUCCESS;
}