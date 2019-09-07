/* 
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission 
    to link the code of portions of this program with the OpenSSL library. 
    You must obey the GNU General Public License in all respects for all 
    of the code used other than OpenSSL. If you modify file(s) with this 
    exception, you may extend this exception to your version of the file(s), 
    but you are not obligated to do so. If you do not wish to do so, delete this 
    exception statement from your version. If you delete this exception statement 
    from all source files in the program, then also delete it here.

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "td/actor/actor.h"
#include "td/utils/buffer.h"
#include "td/utils/port/IPAddress.h"
#include "td/net/UdpServer.h"
#include "td/utils/port/signals.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/FileLog.h"
#include "td/utils/port/path.h"
#include "td/utils/port/user.h"
#include "td/utils/filesystem.h"
#include "common/checksum.h"
#include "common/errorcode.h"
#include "tl-utils/tl-utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "adnl-proxy-types.h"
#include <map>

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif

namespace ton {

namespace adnl {

class Receiver : public td::actor::Actor {
 public:
  void start_up() override;
  void receive_common(td::BufferSlice data);
  void receive_from_client(td::BufferSlice data);
  void receive_to_client(td::BufferSlice data);

  Receiver(td::uint16 in_port, td::uint16 out_port, std::shared_ptr<AdnlProxy> proxy, td::IPAddress client_addr)
      : in_port_(in_port), out_port_(out_port), proxy_(std::move(proxy)), addr_(client_addr) {
  }

 private:
  td::uint16 in_port_;
  td::uint16 out_port_;
  std::shared_ptr<ton::adnl::AdnlProxy> proxy_;
  td::IPAddress addr_;
  td::actor::ActorOwn<td::UdpServer> out_udp_server_;
  td::actor::ActorOwn<td::UdpServer> in_udp_server_;
};

void Receiver::start_up() {
  class Callback : public td::UdpServer::Callback {
   public:
    Callback(td::actor::ActorId<Receiver> manager, td::uint32 mode) : manager_(std::move(manager)), mode_(mode) {
    }

   private:
    td::actor::ActorId<Receiver> manager_;
    const td::uint32 mode_;
    void on_udp_message(td::UdpMessage udp_message) override {
      if (udp_message.error.is_error()) {
        LOG(DEBUG) << udp_message.error;
        return;
      }
      if (mode_ == 0) {
        td::actor::send_closure_later(manager_, &Receiver::receive_common, std::move(udp_message.data));
      } else if (mode_ == 1) {
        td::actor::send_closure_later(manager_, &Receiver::receive_from_client, std::move(udp_message.data));
      } else {
        td::actor::send_closure_later(manager_, &Receiver::receive_to_client, std::move(udp_message.data));
      }
    }
  };

  if (in_port_ == out_port_) {
    auto X = td::UdpServer::create("udp server", in_port_, std::make_unique<Callback>(actor_id(this), 0));
    X.ensure();
    in_udp_server_ = X.move_as_ok();
  } else {
    auto X = td::UdpServer::create("udp server", in_port_, std::make_unique<Callback>(actor_id(this), 1));
    X.ensure();
    in_udp_server_ = X.move_as_ok();
    X = td::UdpServer::create("udp server", out_port_, std::make_unique<Callback>(actor_id(this), 2));
    X.ensure();
    out_udp_server_ = X.move_as_ok();
  }
}

void Receiver::receive_common(td::BufferSlice data) {
  if (data.size() <= 32) {
    return;
  }

  td::Bits256 id;
  id.as_slice().copy_from(data.as_slice().truncate(32));

  if (id.is_zero()) {
    receive_from_client(std::move(data));
  } else {
    receive_to_client(std::move(data));
  }
}

void Receiver::receive_from_client(td::BufferSlice data) {
  auto F = proxy_->decrypt(std::move(data));
  if (F.is_error()) {
    return;
  }
  auto f = F.move_as_ok();

  td::IPAddress a;
  if (a.init_ipv4_port(td::IPAddress::ipv4_to_str(f.ip), f.port).is_error()) {
    return;
  }

  td::UdpMessage M;
  M.address = a;
  M.data = std::move(f.data);

  td::actor::send_closure(out_udp_server_.empty() ? in_udp_server_.get() : out_udp_server_.get(), &td::UdpServer::send,
                          std::move(M));
}

void Receiver::receive_to_client(td::BufferSlice data) {
  LOG(DEBUG) << "proxying to " << addr_;
  td::UdpMessage M;
  M.address = addr_;
  M.data = std::move(data);

  td::actor::send_closure(in_udp_server_.empty() ? out_udp_server_.get() : in_udp_server_.get(), &td::UdpServer::send,
                          std::move(M));
}

}  // namespace adnl

}  // namespace ton

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  td::set_default_failure_signal_handler().ensure();

  std::vector<td::actor::ActorOwn<ton::adnl::Receiver>> x;
  std::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  std::string config = "/var/ton-work/etc/adnl-proxy.conf.json";

  td::OptionsParser p;
  p.set_description("validator or full node for TON network");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
    return td::Status::OK();
  });
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
    return td::Status::OK();
  });
  p.add_option('c', "config", "config file", [&](td::Slice arg) {
    config = arg.str();
    return td::Status::OK();
  });
  p.add_option('d', "daemonize", "set SIGHUP", [&]() {
    td::set_signal_handler(td::SignalType::HangUp, [](int sig) {
#if TD_DARWIN || TD_LINUX
      close(0);
      setsid();
#endif
    }).ensure();
    return td::Status::OK();
  });
#if TD_DARWIN || TD_LINUX
  p.add_option('l', "logname", "log to file", [&](td::Slice fname) {
    auto F = std::make_unique<td::FileLog>();
    TRY_STATUS(F->init(fname.str()));
    logger_ = std::move(F);
    td::log_interface = logger_.get();
    return td::Status::OK();
  });
#endif
  td::uint32 threads = 7;
  p.add_option('t', "threads", PSTRING() << "number of threads (default=" << threads << ")", [&](td::Slice fname) {
    td::int32 v;
    try {
      v = std::stoi(fname.str());
    } catch (...) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: not a number");
    }
    if (v < 1 || v > 256) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: should be in range [1..256]");
    }
    threads = v;
    return td::Status::OK();
  });
  p.add_option('u', "user", "change user", [&](td::Slice user) { return td::change_user(user); });

  p.run(argc, argv).ensure();

  td::actor::Scheduler scheduler({threads});

  auto R = [&]() -> td::Status {
    TRY_RESULT_PREFIX(conf_data, td::read_file(config), "failed to read: ");
    TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");

    ton::ton_api::engine_adnlProxy_config conf;
    TRY_STATUS_PREFIX(ton::ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");

    if (!conf.ports_.size()) {
      return td::Status::Error("empty config");
    }

    for (auto &y : conf.ports_) {
      auto in_port = static_cast<td::uint16>(y->in_port_);
      auto out_port = static_cast<td::uint16>(y->out_port_);
      if (!y->proxy_type_) {
        return td::Status::Error("empty proxy type");
      }
      TRY_RESULT(proxy, ton::adnl::AdnlProxy::create(*y->proxy_type_.get()));
      td::IPAddress a;
      a.init_ipv4_port(td::IPAddress::ipv4_to_str(y->dst_ip_), static_cast<td::uint16>(y->dst_port_)).ensure();

      scheduler.run_in_context([&] {
        x.push_back(td::actor::create_actor<ton::adnl::Receiver>("adnl-proxy", in_port, out_port, std::move(proxy), a));
      });
    }

    return td::Status::OK();
  }();

  if (R.is_error()) {
    LOG(FATAL) << "bad config: " << R.move_as_error();
  }

  while (scheduler.run(1)) {
  }
}
