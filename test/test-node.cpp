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

    Copyright 2017-2020 Telegram Systems LLP
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

#include "crypto/common/refvector.hpp"

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
  std::vector<td::UInt256> ping_ids_;
  td::Timestamp next_dht_dump_;

  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;
  std::vector<td::actor::ActorOwn<ton::dht::Dht>> dht_nodes_;
  td::actor::ActorOwn<ton::overlay::Overlays> overlay_manager_;
  std::vector<std::pair<td::UInt256, td::UInt256>> overlays_;
  std::vector<td::actor::ActorOwn<ton::CatChain>> catchains_;

  std::string local_config_ = "ton-local.config";
  std::string global_config_ = "ton-global.config";

  td::int32 broadcast_size_ = 100;

  void receive_message(td::UInt256 src, td::UInt256 dst, td::BufferSlice data) {
    LOG(ERROR) << "MESSAGE FROM " << src << " to " << dst << " of size " << std::to_string(data.size()) << "\n";
  }

  void receive_broadcast(td::UInt256 overlay_id, td::BufferSlice data) {
    LOG(ERROR) << "BROADCAST IN " << overlay_id << " hash=" << td::sha256(data.as_slice()) << "\n";
  }

  void receive_query(td::UInt256 src, td::UInt256 dst, td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
    auto Q = ton::fetch_tl_object<ton::ton_api::getTestObject>(std::move(data), true);
    CHECK(Q.is_ok());
    auto R = Q.move_as_ok();
    LOG(ERROR) << "QUERY "
               << " FROM " << src << " to " << dst << ": " << ton::ton_api::to_string(R) << "\n";
    promise.set_value(serialize_tl_object(ton::create_tl_object<ton::ton_api::testObject>(), true));
  }

  void catchain_new_block(td::UInt256 src, td::uint64 height, td::BufferSlice data) {
    LOG(ERROR) << "CATCHAIN BLOCK: " << src << "@" << height << ":  " << td::sha256_uint256(data.as_slice()) << "\n";
  }
  void catchain_bad_block(td::UInt256 src) {
    LOG(ERROR) << "CATCHAIN BAD BLOCK\n";
  }
  void catchain_broadcast(td::BufferSlice data) {
    LOG(ERROR) << "CATCHAIN BROADCAST " << td::sha256_uint256(data.as_slice()) << "\n";
  }

  std::unique_ptr<ton::adnl::Adnl::Callback> make_callback() {
    class Callback : public ton::adnl::Adnl::Callback {
     public:
      void receive_message(td::UInt256 src, td::UInt256 dst, td::BufferSlice data) override {
        td::actor::send_closure(id_, &TestNode::receive_message, src, dst, std::move(data));
      }
      void receive_query(td::UInt256 src, td::UInt256 dst, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::send_closure(id_, &TestNode::receive_query, src, dst, std::move(data), std::move(promise));
      }
      Callback(td::actor::ActorId<TestNode> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorId<TestNode> id_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

  std::unique_ptr<ton::CatChainActor::Callback> make_catchain_callback() {
    class Callback : public ton::CatChainActor::Callback {
     public:
      void new_block(td::UInt256 src, td::uint64 height, td::BufferSlice data) override {
        td::actor::send_closure(id_, &TestNode::catchain_new_block, src, height, std::move(data));
      }
      void bad_block(td::UInt256 src) override {
        td::actor::send_closure(id_, &TestNode::catchain_bad_block, src);
      }
      void broadcast(td::BufferSlice data) override {
        td::actor::send_closure(id_, &TestNode::catchain_broadcast, std::move(data));
      }
      Callback(td::actor::ActorId<TestNode> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorId<TestNode> id_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

  std::unique_ptr<ton::overlay::Overlays::Callback> make_overlay_callback() {
    class Callback : public ton::overlay::Overlays::Callback {
     public:
      void receive_message(td::UInt256 src, td::UInt256 overlay_id, td::BufferSlice data) override {
      }
      void receive_query(td::UInt256 src, td::uint64 query_id, td::UInt256 overlay_id, td::BufferSlice data) override {
      }

      void receive_broadcast(td::UInt256 overlay_id, td::BufferSlice data) override {
        td::actor::send_closure(id_, &TestNode::receive_broadcast, overlay_id, std::move(data));
      }
      Callback(td::actor::ActorId<TestNode> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorId<TestNode> id_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

 public:
  void set_broadcast_size(td::int32 size) {
    broadcast_size_ = size;
  }
  void set_local_config(std::string str) {
    local_config_ = str;
  }
  void set_global_config(std::string str) {
    global_config_ = str;
  }
  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(1);
  }
  void alarm() override {
    /*if (overlays_.size() > 0 && broadcast_size_ > 0) {
      td::BufferSlice s(broadcast_size_);
      td::Random::secure_bytes(s.as_slice());

      td::actor::send_closure(overlay_manager_, &ton::overlay::OverlayManager::send_broadcast_fer, overlays_[0].first,
                              overlays_[0].second, ton::create_tl_object<ton::ton_api::testString>(s.as_slice().str()));
    }*/
    for (auto &chain : catchains_) {
      td::BufferSlice s(broadcast_size_);
      td::Random::secure_bytes(s.as_slice());

      td::actor::send_closure(chain, &ton::CatChainActor::add_event, std::move(s));
    }
    alarm_timestamp() = td::Timestamp::in(1.0);
    if (next_dht_dump_.is_in_past()) {
      /*for (auto &node : dht_nodes_) {
        char b[10240];
        td::StringBuilder sb({b, 10000});
        node->get_actor_unsafe().dump(sb);
        LOG(DEBUG) << sb.as_cslice().c_str();
      }*/
      next_dht_dump_ = td::Timestamp::in(60.0);
    }
  }
  TestNode() {
    adnl_ = ton::adnl::Adnl::create("/var/ton-work/db.adnl");
  }
  void run() {
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
        auto R = ton::dht::Dht::create_from_json(
            ton::clone_tl_object(gc.dht_), ton::move_tl_object_as<ton::ton_api::dht_config_local>(it), adnl_.get());
        if (R.is_error()) {
          LOG(FATAL) << "fail creating dht node: " << R.move_as_error();
        }
        dht_nodes_.push_back(R.move_as_ok());
      } else {
        auto I = ton::move_tl_object_as<ton::ton_api::dht_config_random_local>(it);
        for (int i = 0; i < I->cnt_; i++) {
          auto R = ton::dht::Dht::create_random(ton::clone_tl_object(gc.dht_), ton::clone_tl_object(I->addr_list_),
                                                adnl_.get());
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

    overlay_manager_ = ton::overlay::Overlays::create(adnl_.get(), dht_nodes_[0].get());

    for (auto &it : lc.public_overlays_) {
      if (it->get_id() == ton::ton_api::overlay_config_local::ID) {
        auto X = ton::move_tl_object_as<ton::ton_api::overlay_config_local>(it);
        auto id = ton::create_tl_object<ton::ton_api::adnl_id_overlay>(X->name_.clone());
        auto Id = ton::move_tl_object_as<ton::ton_api::adnl_id_Full>(id);
        auto sid = ton::adnl_short_id(Id);
        overlays_.emplace_back(X->id_->id_, sid);
        td::actor::send_closure(overlay_manager_, &ton::overlay::Overlays::create_public_overlay, X->id_->id_,
                                std::move(Id), make_overlay_callback());
      } else {
        auto X = ton::move_tl_object_as<ton::ton_api::overlay_config_random_local>(it);
        for (int i = 0; i < X->cnt_; i++) {
          auto pk = ton::adnl_generate_random_pk();
          auto local_id = ton::adnl_short_id(ton::get_public_key(pk));

          td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, std::move(pk), ton::clone_tl_object(X->addr_list_));

          auto id = ton::create_tl_object<ton::ton_api::adnl_id_overlay>(X->name_.clone());
          auto Id = ton::move_tl_object_as<ton::ton_api::adnl_id_Full>(id);
          auto sid = ton::adnl_short_id(Id);
          overlays_.emplace_back(local_id, sid);
          td::actor::send_closure(overlay_manager_, &ton::overlay::Overlays::create_public_overlay, local_id,
                                  std::move(Id), make_overlay_callback());
        }
      }
    }

    //auto C = ton::CatChainActor::create(nullptr, adnl_.get(), overlay_manager_.get(),
    //                                    std::vector<ton::tl_object_ptr<ton::ton_api::adnl_id_Full>>());

    for (auto &it : lc.catchains_) {
      auto tag = it->tag_;
      for (auto &V : gc.catchains_) {
        if (V->tag_ == tag) {
          auto v = std::move(clone_tl_object(V)->nodes_);
          auto C = ton::CatChainActor::create(make_catchain_callback(), adnl_.get(), overlay_manager_.get(),
                                              std::move(v), it->id_->id_, tag);
          catchains_.push_back(std::move(C));
        }
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
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);
  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<TestNode> x;

  td::OptionsParser p;
  p.set_description("test basic adnl functionality");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb({b, 10000});
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
  p.add_option('s', "broadcast-size", "size of broadcast", [&](td::Slice fname) {
    td::actor::send_closure(x, &TestNode::set_broadcast_size, std::atoi(fname.str().c_str()));
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
