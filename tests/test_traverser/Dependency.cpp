#include <cstdlib>
#include <ctime>
#include <iostream>
#include <stack>

#include "../../examples/Utils.hpp"
#include "../../examples/knn/KnnSet.hpp"

template <typename T>
struct TreeNode {
  T val;
  TreeNode<T>* left;
  TreeNode<T>* right;

  explicit TreeNode(const T x) : val(x), left(), right() {}

  _NODISCARD bool IsLeaf() const { return left == nullptr && right == nullptr; }
};

template <typename T>
TreeNode<T>* insert(TreeNode<T>* root, const T val) {
  if (root == nullptr) return new TreeNode(val);

  if (val < root->val)
    root->left = insert(root->left, val);
  else
    root->right = insert(root->right, val);

  return root;
}

template <typename DataT, typename ResultT>
ResultT Distance(const DataT p, const DataT q) {
  ResultT dx = p - q;
  return sqrtf(dx * dx);
}

enum class ExecutionState { kWorking, kFinished };

template <typename DataT, typename ResultT>
class Executor {
  std::stack<TreeNode<DataT>*> node_stack_;
  TreeNode<DataT>* cur_;

  DataT q_;
  KnnSet<float, 1> k_set_;

  ExecutionState state_ = ExecutionState::kFinished;

 public:
  void StartTraversal(TreeNode<DataT>* root, const DataT& q) {
    cur_ = root;
    q_ = q;
    k_set_.Clear();
    Execute();
  }

  void StartCpuTraversal(TreeNode<DataT>* root, const DataT& q) {
    k_set_.Clear();
    InorderTraversal(root, q);
  }

  void Resume() { Execute(); }

  _NODISCARD bool Finished() const {
    return state_ == ExecutionState::kFinished;
  }

 private:
  void InorderTraversal(const TreeNode<DataT>* cur, const DataT& q) {
    if (cur == nullptr) return;

    if (cur->IsLeaf()) {
      // **** pre ****
      std::cout << "leaf " << cur->val << std::endl;
      const auto dist = Distance<DataT, ResultT>(cur_->val, q_);
      k_set_.Insert(dist);
      // *************

    } else {
      std::cout << "branch pre " << cur->val << std::endl;

      // **** branch pre ****

      // ********************

      InorderTraversal(cur->left, q);

      std::cout << "branch in " << cur->val << std::endl;

      InorderTraversal(cur->right, q);
    }
  }

  void Execute() {
    if (state_ == ExecutionState::kWorking) goto my_resume_point;
    state_ = ExecutionState::kWorking;

    while (cur_ != nullptr || !node_stack_.empty()) {
      while (cur_ != nullptr) {
        if (cur_->IsLeaf()) {
          // **** pre ****
          std::cout << "leaf " << cur_->val << std::endl;
          // *************

          // **** Coroutine return (API) ****
          return;
        my_resume_point:
          // ****************************

          cur_ = nullptr;
          continue;
        }

        // **** branch pre ****
        std::cout << "branch pre " << cur_->val << std::endl;
        // *******************

        node_stack_.push(cur_);
        cur_ = cur_->left;
      }

      if (!node_stack_.empty()) {
        cur_ = node_stack_.top();
        node_stack_.pop();

        // **** branch in ****
        std::cout << "branch in " << cur_->val << std::endl;
        // *******************

        cur_ = cur_->right;
      }
    }

    state_ = ExecutionState::kFinished;
  }
};

int main() {
  constexpr int n = 32;
  srand(666);

  TreeNode<float>* root = nullptr;

  for (int i = 0; i < n; i++) {
    const float val = rand() % 1000;
    root = insert(root, val);
  }

  std::cout << "Inorder Traversal of the Binary Search Tree:\n";

  float q = 0.5f;

  Executor<float, float> a{};

  a.StartCpuTraversal(root, q);

  std::cout << "-----------------------" << std::endl;

  Executor<float, float> exe{};

  exe.StartTraversal(root, q);

  while (!exe.Finished()) {
    exe.Resume();
  }

  return 0;
}
