// Copyright 2019, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "util/fibers/fiberqueue_threadpool.h"

#include "absl/strings/str_cat.h"
#include "base/pthread_utils.h"

namespace util {
namespace fibers_ext {
using namespace boost;
using namespace std;

FiberQueueThreadPool::FiberQueueThreadPool(unsigned num_threads, unsigned queue_size)
    : input_(queue_size) {
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
  }

  for (unsigned i = 0; i < num_threads; ++i) {
    string name = absl::StrCat("sq_threadpool", i);

    auto fn = std::bind(&FiberQueueThreadPool::WorkerFunction, this);
    workers_.emplace_back(base::StartThread(name.c_str(), fn));
  }
}

FiberQueueThreadPool::~FiberQueueThreadPool() {
  VLOG(1) << "FiberQueueThreadPool::~FiberQueueThreadPool";

  if (!workers_.empty()) {
    Shutdown();
  }
}

void FiberQueueThreadPool::Shutdown() {
  VLOG(1) << "FiberQueueThreadPool::ShutdownStart";

  input_.close();
  for (auto& w : workers_) {
    pthread_join(w, nullptr);
  }
  workers_.clear();
}


void FiberQueueThreadPool::WorkerFunction() {
  while (true) {
    Func f;
    fibers::channel_op_status st = input_.pop(f);
    if (st == fibers::channel_op_status::closed)
      break;

    try {
      f();
    } catch (std::exception& e) {
      // std::exception_ptr p = std::current_exception();
      LOG(FATAL) << "Exception " << e.what();
    }
  }
}

}  // namespace fibers_ext
}  // namespace util
