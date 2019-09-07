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
#include "adnl/utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "dht/dht.h"
#include "overlay/overlays.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/Random.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/FileFd.h"
#include "catchain/catchain.h"
#include "validator-session/validator-session.h"
#include "ton-node/ton-node.h"
#include "validator/manager.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/path.h"

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
  std::vector<td::actor::ActorOwn<ton::dht::Dht>> dht_nodes_;
  td::actor::ActorOwn<ton::overlay::Overlays> overlay_manager_;
  td::actor::ActorOwn<ton::ValidatorManager> validator_manager_;
  td::actor::ActorOwn<ton::TonNodeManager> ton_node_;
  std::vector<td::actor::ActorOwn<ton::adnl::AdnlFileTransfer>> file_transfers_;

  std::string local_config_ = "ton-local.config";
  std::string global_config_ = "ton-global.config";

  std::string inst_id_ = "";
  std::string db_root_ = "/var/ton-work/db/";

  std::unique_ptr<ton::adnl::Adnl::Callback> make_callback() {
    class Callback : public ton::adnl::Adnl::Callback {
     public:
      void receive_message(td::UInt256 src, td::UInt256 dst, td::BufferSlice data) override {
        td::actor::send_closure(id_, &TestNode::adnl_receive_message, src, dst, std::move(data));
      }
      void receive_query(td::UInt256 src, td::UInt256 dst, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
      }
      Callback(td::actor::ActorId<TestNode> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorId<TestNode> id_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

 public:
  void adnl_receive_message(td::UInt256 src, td::UInt256 dst, td::BufferSlice data) {
    LOG(ERROR) << "ADNL MESSAGE FROM " << src << ": size=" << data.size() << "\n";
  }

  void set_local_config(std::string str) {
    local_config_ = str;
  }
  void set_local_id(std::string id) {
    inst_id_ = id;
  }
  void set_global_config(std::string str) {
    global_config_ = str;
  }
  void set_db_root(std::string db_root) {
    db_root_ = db_root;
  }
  void start_up() override {
  }
  void alarm() override {
  }
  TestNode() {
  }
  void run() {
    td::mkdir(db_root_).ensure();

    auto L = td::read_file(local_config_).move_as_ok();
    auto lc_j = td::json_decode(L.as_slice()).move_as_ok();
    ton::ton_api::config_local lc;
    ton::ton_api::from_json(lc, lc_j.get_object()).ensure();

    auto G = td::read_file(global_config_).move_as_ok();
    auto gc_j = td::json_decode(G.as_slice()).move_as_ok();
    ton::ton_api::config_global gc;
    ton::ton_api::from_json(gc, gc_j.get_object()).ensure();

    CHECK(lc.dummy0_.size() == 1);
    CHECK(gc.dummy0_.size() == 1);

    validator_manager_ = ton::ValidatorManagerFactory::create(lc.dummy0_[0]->id_->id_, gc.dummy0_[0]->zero_state_hash_,
                                                              gc.dummy0_[0]->zero_state_hash_, db_root_, adnl_.get());
    class Callback : public ton::ValidatorManager::Callback {
     private:
      td::actor::ActorId<ton::ValidatorManager> id_;

     public:
      Callback(td::actor::ActorId<ton::ValidatorManager> id) : id_(id) {
      }

      void initial_read_complete(ton::WorkchainId workchain, ton::ShardId shard, ton::adnl::AdnlNodeIdShort who,
                                 std::vector<ton::BlockHandle> top_blocks) override {
        td::actor::send_closure(id_, &ton::ValidatorManager::sync_complete, workchain, shard,
                                td::PromiseCreator::lambda([](td::Unit) {}));
      }
      void new_ihr_message(ton::WorkchainId workchain, ton::adnl::AdnlNodeIdShort who, td::UInt256 dst,
                           td::BufferSlice data) override {
      }
      void download_block(ton::BlockIdExt block_id, td::Timestamp timeout, ton::adnl::AdnlNodeIdShort who,
                          td::Promise<ton::ReceivedBlock> promise) override {
      }
    };

    td::actor::send_closure(validator_manager_, &ton::ValidatorManager::install_callback,
                            std::make_unique<Callback>(validator_manager_.get()),
                            td::PromiseCreator::lambda([](td::Unit) {}));
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
  SET_VERBOSITY_LEVEL(verbosity_INFO);
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
  p.add_option('i', "id", "id of instance", [&](td::Slice fname) {
    td::actor::send_closure(x, &TestNode::set_local_id, fname.str());
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

  td::actor::Scheduler scheduler({7});
  scheduler.run_in_context([&] { x = td::actor::create_actor<TestNode>("testnode"); });

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] { td::actor::send_closure(x, &TestNode::run); });
  scheduler.run();

  return 0;
}
