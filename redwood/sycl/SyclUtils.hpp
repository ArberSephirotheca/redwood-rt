#pragma once

#include <CL/sycl.hpp>

constexpr auto kNumStreams = 2;

extern sycl::device device;
extern sycl::context ctx;
extern sycl::queue qs[kNumStreams];

static auto exception_handler = [](sycl::exception_list eList) {
  for (const std::exception_ptr& e : eList) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& e) {
#if DEBUG
      std::cout << "Failure" << std::endl;
#endif
      std::terminate();
    }
  }
};