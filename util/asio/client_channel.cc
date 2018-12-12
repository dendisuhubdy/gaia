// Copyright 2018, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/asio/client_channel.h"
#include <boost/asio/connect.hpp>
#include <boost/fiber/fiber.hpp>

#include "base/logging.h"

using namespace std;
using chrono::milliseconds;
using chrono::steady_clock;
using namespace boost;
using namespace asio::ip;

namespace util {

namespace detail {
ClientChannelImpl::~ClientChannelImpl() {
}

system::error_code ClientChannelImpl::Connect(uint32_t ms) {
  CHECK(!shutting_down_ && !reconnect_active_);

  // If we are connected
  if (!status_)
    return status_;

  VLOG(1) << "Connecting on socket " << sock_.native_handle() << " " << sock_.non_blocking()
          << "hostname: " << hostname_ << ", service: " << service_;

  time_point tp = steady_clock::now() + milliseconds(ms);
  fibers_ext::Done done;

  // TODO: to introduce synchronous Fiber launching method in IoContext.
  // I prefer moving the work to IoContext thread to keep all the data-structure updates as
  // single-threaded. It seems that Asio components (timer, for example) in rare cases have
  // data-races. For example, if I call timer.cancel() in one thread but wait on it in another
  // it can ignore "cancel()" call in some cases.
  io_context_.Post([&] {
    auto cb = [&] {
      this->ResolveAndConnect(tp);
      done.Notify();
    };
    fibers::fiber(std::move(cb)).detach();
  });
  done.Wait();

  return status_;
}

void ClientChannelImpl::ResolveAndConnect(const time_point& until) {
  auto sleep_dur = 100ms;
  VLOG(1) << "ClientChannel::ResolveAndConnect " << sock_.native_handle();

  asio::steady_timer timer(sock_.get_executor().context());
  fibers_ext::Done done;

  auto connect_cb = [&](const system::error_code& ec, const tcp::endpoint& ep) {
    VLOG(1) << "Async connect with status " << ec << " " << ec.message();
    timer.cancel();

    if (!ec) {
      sock_.non_blocking(true);

      VLOG(1) << "Connected to endpoint " << ep;
    }
    status_ = ec;
    done.Notify();
  };

  while (!shutting_down_ && status_ && steady_clock::now() < until) {
    DCHECK(status_);

    system::error_code resolve_ec;
    auto results = Resolve(fibers_ext::yield[resolve_ec]);
    if (!resolve_ec) {  // OK
      system::error_code ec;
      VLOG(2) << "Connecting to " << results.size() << " endpoints";

      done.Reset();
      timer.expires_at(until);  // Alarm the timer.
      asio::async_connect(sock_, results, connect_cb);

      timer.async_wait(fibers_ext::yield[ec]);
      if (status_ && !ec) {
        // Timer successfully fired, and status_ is still not ok, so cancel async_connect operation.
        sock_.cancel();
      } else if (!status_) {
        VLOG(2) << "Connected, timer status: " << ec.message();
        return;  // connected.
      }
      done.Wait();  // wait for connect_cb to run.
    }

    time_point now = steady_clock::now();
    if (shutting_down_ || now + 2ms >= until) {
      status_ = asio::error::operation_aborted;
      return;
    }

    time_point sleep_until = std::min(now + sleep_dur, until - 2ms);
    timer.expires_at(sleep_until);
    timer.async_wait(fibers_ext::yield[resolve_ec]);
    if (sleep_dur < 1s)
      sleep_dur += 100ms;
  }
}

void ClientChannelImpl::Shutdown() {
  if (!shutting_down_) {
    system::error_code ec;

    shutting_down_ = true;
    // I had crashes when resolver_ was executing in one thread and cancel has been called from
    // here. Lets hope resolver does not take time to finish. resolver_.cancel();

    VLOG(1) << "Cancelling " << sock_.native_handle();
    sock_.shutdown(tcp::socket::shutdown_both, ec);
    if (!ec) {
      sock_.cancel(ec);
      LOG_IF(INFO, ec) << "Cancelled with status " << ec << " " << ec.message();
    }
    VLOG(1) << "ClientChannelImpl::Shutdown end " << sock_.native_handle() << " " << ec.message();
  }
  std::unique_lock<mutex> l(shd_mu_);
  shd_cnd_.wait(l, [this] { return !reconnect_active_; });
}

void ClientChannelImpl::ReconnectFiber() {
  this_fiber::properties<IoFiberProperties>().SetNiceLevel(4);

  ResolveAndConnect(steady_clock::now() + 30s);
  DCHECK(reconnect_active_);

  if (!shutting_down_ && status_) {
    ReconnectAsync();  // continue trying.
    return;
  }

  std::lock_guard<mutex> guard(shd_mu_);

  reconnect_active_ = false;

  if (shutting_down_) {
    shd_cnd_.notify_one();
  } else {
    DCHECK(!status_);

    LOG(INFO) << "Socket " << sock_.native_handle() << " reconnected";
  }
}

void ClientChannelImpl::HandleErrorStatus() {
  if (shutting_down_ || reconnect_active_) {
    return;
  }
  LOG(INFO) << "Got " << status_.message() << ", reconnecting " << sock_.native_handle();

  std::lock_guard<mutex> guard(shd_mu_);

  reconnect_active_ = true;

  ReconnectAsync();
}

}  // namespace detail

ClientChannel::~ClientChannel() {
  Shutdown();
}

}  // namespace util
