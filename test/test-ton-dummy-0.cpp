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
#include "crypto/vm/cp0.h"

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
  td::actor::ActorOwn<ton::Keyring> keyring_;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;
  td::actor::ActorOwn<ton::rldp::Rldp> rldp_;
  std::vector<td::actor::ActorOwn<ton::dht::Dht>> dht_nodes_;
  td::actor::ActorOwn<ton::overlay::Overlays> overlay_manager_;
  td::actor::ActorOwn<ton::ValidatorManager> validator_manager_;
  td::actor::ActorOwn<ton::TonNodeManager> ton_node_;

  std::string local_config_ = "ton-local.config";
  std::string global_config_ = "ton-global.config";

  std::string db_root_ = "/var/ton-work/db/";
  std::string zero_state_ = "";

 public:
  void set_local_config(std::string str) {
    local_config_ = str;
  }
  void set_global_config(std::string str) {
    global_config_ = str;
  }
  void set_db_root(std::string db_root) {
    db_root_ = db_root;
  }
  void set_zero_state(std::string zero_state) {
    zero_state_ = zero_state;
  }
  void start_up() override {
  }
  void alarm() override {
  }
  TestNode() {
  }
  void run() {
    td::mkdir(db_root_).ensure();

    keyring_ = ton::Keyring::create();
    adnl_ = ton::adnl::Adnl::create(db_root_, keyring_.get());

    auto L = td::read_file(local_config_).move_as_ok();
    auto lc_j = td::json_decode(L.as_slice()).move_as_ok();
    ton::ton_api::config_local lc;
    ton::ton_api::from_json(lc, lc_j.get_object()).ensure();

    auto G = td::read_file(global_config_).move_as_ok();
    auto gc_j = td::json_decode(G.as_slice()).move_as_ok();
    ton::ton_api::config_global gc;
    ton::ton_api::from_json(gc, gc_j.get_object()).ensure();

    for (auto &port : lc.udp_ports_) {
      td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_listening_udp_port, "0.0.0.0",
                              static_cast<td::uint16>(port));
    }
    /*if (!lc.net_) {
      LOG(FATAL) << "local config does not contain NET section";
    }*/

    //td::actor::send_closure(network_manager_, &ton::adnl::AdnlNetworkManager::load_local_config, std::move(lc.net_));
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_ids_from_config, std::move(lc.local_ids_));
    if (gc.adnl_) {
      td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config,
                              std::move(gc.adnl_->static_nodes_));
    }
    if (!gc.dht_) {
      LOG(FATAL) << "global config does not contain dht section";
    }

    for (auto &it : lc.dht_) {
      if (it->get_id() == ton::ton_api::dht_config_local::ID) {
        auto R = ton::dht::Dht::create_from_json(ton::clone_tl_object(gc.dht_),
                                                 ton::move_tl_object_as<ton::ton_api::dht_config_local>(it),
                                                 keyring_.get(), adnl_.get());
        if (R.is_error()) {
          LOG(FATAL) << "fail creating dht node: " << R.move_as_error();
        }
        dht_nodes_.push_back(R.move_as_ok());
      } else {
        auto I = ton::move_tl_object_as<ton::ton_api::dht_config_random_local>(it);
        for (int i = 0; i < I->cnt_; i++) {
          auto R = ton::dht::Dht::create_random(ton::clone_tl_object(gc.dht_), ton::clone_tl_object(I->addr_list_),
                                                keyring_.get(), adnl_.get());
          if (R.is_error()) {
            LOG(FATAL) << "fail creating dht node: " << R.move_as_error();
          }
          dht_nodes_.push_back(R.move_as_ok());
        }
      }
    }

    CHECK(dht_nodes_.size() > 0);

    td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_dht_node, dht_nodes_[0].get());
    //td::actor::send_closure(overlay_manager_, &ton::overlay::Overlays::register_dht_node, dht_nodes_[0].get());

    overlay_manager_ = ton::overlay::Overlays::create(keyring_.get(), adnl_.get(), dht_nodes_[0].get());

    //auto C = ton::CatChainActor::create(nullptr, adnl_.get(), overlay_manager_.get(),
    //                                    std::vector<ton::tl_object_ptr<ton::ton_api::adnl_id_Full>>());

    CHECK(lc.dummy0_.size() <= 1);
    CHECK(gc.dummy0_.size() <= 1);

    if (lc.dummy0_.size() == 1) {
      CHECK(gc.dummy0_.size() == 1);
      auto zero_state_id = ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0,
                                           ton::UInt256_2_Bits256(gc.dummy0_[0]->zero_state_hash_),
                                           ton::UInt256_2_Bits256(gc.dummy0_[0]->zero_state_hash_)};
      validator_manager_ = ton::ValidatorManagerFactory::create(
          ton::PublicKeyHash{lc.dummy0_[0]->id_->id_}, zero_state_id, "", zero_state_,
          {ton::ShardIdFull{ton::basechainId, ton::shardIdAll}}, db_root_, keyring_.get(), adnl_.get(), rldp_.get(),
          overlay_manager_.get());
      ton_node_ =
          ton::TonNodeManager::create(ton::adnl::AdnlNodeIdShort{lc.dummy0_[0]->id_->id_}, adnl_.get(), rldp_.get(),
                                      dht_nodes_[0].get(), overlay_manager_.get(), validator_manager_.get(), db_root_);

      for (auto &x : lc.liteservers_) {
        auto pk = ton::PrivateKey{x->id_};
        auto pub_k = ton::adnl::AdnlNodeIdFull{pk.compute_public_key()};
        auto id = pub_k.compute_short_id();

        td::actor::send_closure(keyring_, &ton::Keyring::add_key, std::move(pk));
        td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, pub_k, ton::adnl::AdnlAddressList{});
        td::actor::send_closure(validator_manager_, &ton::ValidatorManager::add_ext_server_id, id);
        td::actor::send_closure(validator_manager_, &ton::ValidatorManager::add_ext_server_port,
                                static_cast<td::uint16>(x->port_));
      }
    }
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

  CHECK(vm::init_op_cp0());

  td::actor::ActorOwn<TestNode> x;

  td::OptionsParser p;
  p.set_description("test basic adnl functionality");
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
  p.add_option('C', "global-config", "file to read global config", [&](td::Slice fname) {
    td::actor::send_closure(x, &TestNode::set_global_config, fname.str());
    return td::Status::OK();
  });
  p.add_option('c', "local-config", "file to read local config", [&](td::Slice fname) {
    td::actor::send_closure(x, &TestNode::set_local_config, fname.str());
    return td::Status::OK();
  });
  p.add_option('i', "id", "id of instance", [&](td::Slice fname) { return td::Status::OK(); });
  p.add_option('D', "db", "root for dbs", [&](td::Slice fname) {
    td::actor::send_closure(x, &TestNode::set_db_root, fname.str());
    return td::Status::OK();
  });
  p.add_option('z', "zero-state", "file with serialized zero state", [&](td::Slice fname) {
    td::actor::send_closure(x, &TestNode::set_zero_state, fname.str());
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
