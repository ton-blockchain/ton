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
#include "adnl/adnl.h"
#include "adnl/adnl-ext-client.h"
#include "adnl/utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/Random.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/FileFd.h"
#include "terminal/terminal.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <iostream>
#include <sstream>

template <std::size_t size>
std::ostream &operator<<(std::ostream &stream, const td::UInt<size> &x) {
  for (size_t i = 0; i < size / 8; i++) {
    stream << td::format::hex_digit((x.raw[i] >> 4) & 15) << td::format::hex_digit(x.raw[i] & 15);
  }

  return stream;
}

class TestNode : public td::actor::Actor {
 private:
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;

  std::string local_config_ = "ton-local.config";
  std::string global_config_ = "ton-global.config";

  td::actor::ActorOwn<ton::adnl::AdnlExtClient> client_;
  td::actor::ActorOwn<td::TerminalIO> io_;

  std::unique_ptr<ton::adnl::AdnlExtClient::Callback> make_callback() {
    class Callback : public ton::adnl::AdnlExtClient::Callback {
     public:
      void on_ready() override {
        td::actor::send_closure(id_, &TestNode::conn_ready);
      }
      void on_stop_ready() override {
        td::actor::send_closure(id_, &TestNode::conn_closed);
      }
      Callback(td::actor::ActorId<TestNode> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorId<TestNode> id_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

  bool ready_ = false;
  std::string db_root_;

 public:
  void conn_ready() {
    LOG(ERROR) << "conn ready";
    ready_ = true;
  }
  void conn_closed() {
    ready_ = false;
  }
  void set_local_config(std::string str) {
    local_config_ = str;
  }
  void set_global_config(std::string str) {
    global_config_ = str;
  }
  void set_db_root(std::string db_root) {
    db_root_ = db_root;
  }
  void start_up() override {
    class Cb : public td::TerminalIO::Callback {
     public:
      void line_cb(td::BufferSlice line) override {
        LOG(ERROR) << "read line";
        td::actor::send_closure(id_, &TestNode::send_query, std::move(line));
      }
      Cb(td::actor::ActorId<TestNode> id) : id_(id) {
      }

     private:
      td::actor::ActorId<TestNode> id_;
    };
    io_ = td::TerminalIO::create("", false, std::make_unique<Cb>(actor_id(this)));
    td::actor::send_closure(io_, &td::TerminalIO::set_log_interface);
  }
  void send_query(td::BufferSlice data) {
    if (ready_ && !client_.empty()) {
      LOG(ERROR) << "sending query";
      auto P = td::PromiseCreator::lambda([](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          LOG(ERROR) << "failed query: " << R.move_as_error();
          return;
        }
        auto F = ton::fetch_tl_object<ton::ton_api::Object>(R.move_as_ok(), true);
        if (F.is_error()) {
          LOG(ERROR) << "failed to parse answer: " << F.move_as_error();
          return;
        }
        auto obj = F.move_as_ok();
        LOG(ERROR) << "got answer: " << ton::ton_api::to_string(obj);
      });
      td::BufferSlice b =
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::liteServer_query>(std::move(data)), true);
      td::actor::send_closure(client_, &ton::adnl::AdnlExtClient::send_query, "query", std::move(b),
                              td::Timestamp::in(10.0), std::move(P));
    }
  }
  TestNode() {
  }
  void run() {
    adnl_ = ton::adnl::Adnl::create(db_root_);

    auto G = td::read_file(global_config_).move_as_ok();
    auto gc_j = td::json_decode(G.as_slice()).move_as_ok();
    ton::ton_api::config_global gc;
    ton::ton_api::from_json(gc, gc_j.get_object()).ensure();

    //td::actor::send_closure(network_manager_, &ton::adnl::AdnlNetworkManager::load_local_config, std::move(lc.net_));
    if (gc.adnl_) {
      td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config,
                              std::move(gc.adnl_->static_nodes_));
    }

    CHECK(gc.liteclients_.size() > 0);
    auto &cli = gc.liteclients_[0];
    td::IPAddress addr;
    addr.init_host_port(td::IPAddress::ipv4_to_str(cli->ip_), cli->port_).ensure();
    client_ = ton::adnl::AdnlExtClient::create(ton::adnl::AdnlNodeIdFull{cli->id_}, addr, make_callback());
  }
};

td::Result<td::UInt256> get_uint256(std::string str) {
  if (str.size() != 64) {
    return td::Status::Error("uint256 must have 64 bytes");
  }
  td::UInt256 res;
  for (size_t i = 0; i < 32; i++) {
    res.raw[i] = static_cast<td::uint8>(td::hex_to_int(str[2 * i]) * 16 + td::hex_to_int(str[2 * i + 1]));
  }
  return res;
}

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);
  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<TestNode> x;

  td::OptionsParser p;
  p.set_description("test basic adnl functionality");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
    return td::Status::OK();
  });
  p.add_option('C', "global-config", "file to read global config", [&](td::Slice fname) {
    td::actor::send_closure(x, &TestNode::set_global_config, fname.str());
    return td::Status::OK();
  });
  p.add_option('c', "local-config", "file to read local config", [&](td::Slice fname) {
    td::actor::send_closure(x, &TestNode::set_local_config, fname.str());
    return td::Status::OK();
  });
  p.add_option('D', "db", "root for dbs", [&](td::Slice fname) {
    td::actor::send_closure(x, &TestNode::set_db_root, fname.str());
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
    auto FileLog = td::FileFd::open(td::CSlice(fname.str().c_str()),
                                    td::FileFd::Flags::Create | td::FileFd::Flags::Append | td::FileFd::Flags::Write)
                       .move_as_ok();

    dup2(FileLog.get_native_fd().fd(), 1);
    dup2(FileLog.get_native_fd().fd(), 2);
    return td::Status::OK();
  });
#endif

  td::actor::Scheduler scheduler({2});

  scheduler.run_in_context([&] { x = td::actor::create_actor<TestNode>("testnode"); });

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] { td::actor::send_closure(x, &TestNode::run); });
  scheduler.run();

  return 0;
}
