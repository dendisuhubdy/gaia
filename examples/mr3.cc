// Copyright 2019, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include <boost/fiber/buffered_channel.hpp>

#include "base/init.h"
#include "base/logging.h"

#include "file/fiber_file.h"
#include "file/file_util.h"
#include "file/filesource.h"
#include "mr/mr.h"

#include "util/asio/io_context_pool.h"
#include "util/status.h"

using namespace std;
using namespace boost;
using namespace util;

using fibers::channel_op_status;

DEFINE_string(input, "", "");

namespace mr3 {

StringStream& Pipeline::ReadText(const string& name, const string& glob) {
  std::unique_ptr<InputBase> ib(new InputBase(name, pb::WireFormat::TXT));
  ib->mutable_msg()->add_file_spec()->set_url_glob(glob);
  inputs_.emplace_back(std::move(ib));

  streams_.emplace_back(new StringStream(name));
  auto& ptr = streams_.back();

  return *ptr;
}

using util::Status;

Status Pipeline::Run() { return Status::OK; }

class Executor {
  struct PerIoStruct {
    fibers::fiber process_fd;
  };

  using StringQueue = ::boost::fibers::buffered_channel<string>;

 public:
  Executor(util::IoContextPool* pool)
      : pool_(pool), file_name_q_(16), fq_pool_(new util::FiberQueueThreadPool()) {}

  void Run(const InputBase* input, const StringStream& ss);

 private:
  void ProcessFiles(pb::WireFormat::Type tp);
  void ProcessText(file::ReadonlyFile* fd);

  util::IoContextPool* pool_;
  StringQueue file_name_q_;
  static thread_local std::unique_ptr<PerIoStruct> per_io_;
  std::unique_ptr<util::FiberQueueThreadPool> fq_pool_;
};

thread_local std::unique_ptr<Executor::PerIoStruct> Executor::per_io_;

void Executor::Run(const InputBase* input, const StringStream& ss) {
  CHECK(input && input->msg().file_spec_size() > 0);
  CHECK(input->msg().has_format());
  LOG(INFO) << "Running on input " << input->msg().name();

  pool_->AwaitOnAll([this, input](IoContext&) {
    per_io_.reset(new PerIoStruct);
    per_io_->process_fd =
        fibers::fiber(&Executor::ProcessFiles, this, input->msg().format().type());
  });

  for (const auto& file_spec : input->msg().file_spec()) {
    std::vector<file_util::StatShort> paths = file_util::StatFiles(file_spec.url_glob());
    for (const auto& v : paths) {
      if (v.st_mode & S_IFREG) {
        channel_op_status st = file_name_q_.push(v.name);
        CHECK_EQ(channel_op_status::success, st);
      }
    }
  }
}

void Executor::ProcessFiles(pb::WireFormat::Type tp) {
  string file_name;

  while (true) {
    channel_op_status st = file_name_q_.pop(file_name);
    if (st == channel_op_status::closed)
      break;

    CHECK(st == channel_op_status::success);
    CONSOLE_INFO << file_name;
    auto res = file::OpenFiberReadFile(file_name, fq_pool_.get());
    if (!res.ok()) {
      LOG(INFO) << "Skipping " << file_name << " with " << res.status;
      continue;
    }
    LOG(INFO) << "Processing file " << file_name;

    switch (tp) {
      case pb::WireFormat::TXT:
        ProcessText(res.obj);
        break;

      default:
        LOG(FATAL) << "Not implemented " << pb::WireFormat::Type_Name(tp);
        break;
    }
  }
}

void Executor::ProcessText(file::ReadonlyFile* fd) {
  util::Source* src = file::Source::Uncompressed(fd);
  file::LineReader lr(src, TAKE_OWNERSHIP);
  StringPiece result;
  string scratch;

  while (lr.Next(&result, &scratch)) {
  }
}

}  // namespace mr3

using namespace mr3;
using namespace util;

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);

  CHECK(!FLAGS_input.empty());

  IoContextPool pool;
  Pipeline p;

  // TODO: Should return Input<string> or something which can apply an operator.
  StringStream& ss = p.ReadText("inp1", FLAGS_input);
  ss.Write("outp1").AndCompress(pb::Output::GZIP).WithSharding([](const string& str) {
    return "shardname";
  });

  return 0;
}