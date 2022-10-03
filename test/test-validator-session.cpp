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
#include "rldp/rldp.h"
#include "adnl/utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "dht/dht.h"
#include "overlay/overlays.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/Random.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/FileFd.h"
#include "td/utils/overloaded.h"
#include "catchain/catchain.h"
#include "validator-session/validator-session.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <iostream>
#include <sstream>

class TestNode : public td::actor::Actor {
 private:
  td::actor::ActorOwn<ton::keyring::Keyring> keyring_;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;
  td::actor::ActorOwn<ton::rldp::Rldp> rldp_;
  std::vector<td::actor::ActorOwn<ton::dht::Dht>> dht_nodes_;
  td::actor::ActorOwn<ton::overlay::Overlays> overlay_manager_;
  std::vector<td::actor::ActorOwn<ton::validatorsession::ValidatorSession>> validator_sessions_;

  std::string local_config_ = "ton-local.config";
  std::string global_config_ = "ton-global.config";

  std::unique_ptr<ton::validatorsession::ValidatorSession::Callback> make_vs_callback() {
    class Callback : public ton::validatorsession::ValidatorSession::Callback {
     public:
      void on_candidate(td::uint32 round, ton::PublicKeyHash source,
                        ton::validatorsession::ValidatorSessionRootHash root_hash, td::BufferSlice data,
                        td::BufferSlice extra,
                        td::Promise<ton::validatorsession::ValidatorSession::CandidateDecision> promise) override {
        td::actor::send_closure(id_, &TestNode::on_candidate, round, source, root_hash, std::move(data),
                                std::move(extra), std::move(promise));
      }
      void on_generate_slot(td::uint32 round, td::Promise<ton::BlockCandidate> promise) override {
        td::actor::send_closure(id_, &TestNode::on_generate_slot, round, std::move(promise));
      }
      void on_block_committed(td::uint32 round, ton::PublicKeyHash src,
                              ton::validatorsession::ValidatorSessionRootHash root_hash,
                              ton::validatorsession::ValidatorSessionFileHash file_hash, td::BufferSlice data,
                              std::vector<std::pair<ton::PublicKeyHash, td::BufferSlice>> signatures,
                              ton::validatorsession::ValidatorSessionStats stats) override {
        td::actor::send_closure(id_, &TestNode::on_block_committed, round, root_hash, std::move(data),
                                std::move(signatures));
      }
      /*void on_missing_block_committed(
          td::uint32 round, ton::validatorsession::ValidatorSessionRootHash root_hash, ton::validatorsession::ValidatorSessionFileHash file_hash,
          td::BufferSlice data, std::vector<std::pair<ton::adnl::AdnlNodeIdShort, td::BufferSlice>> signatures) override {
        td::actor::send_closure(id_, &TestNode::on_block_committed_abscent, round, root_hash, file_hash,
                                std::move(data), std::move(signatures));
      }*/
      void on_block_skipped(td::uint32 round) override {
        td::actor::send_closure(id_, &TestNode::on_block_skipped, round);
      }
      void get_approved_candidate(ton::validatorsession::ValidatorSessionRootHash root_hash,
                                  ton::validatorsession::ValidatorSessionFileHash file_hash,
                                  ton::validatorsession::ValidatorSessionFileHash collated_data_file_hash,
                                  td::Promise<ton::BlockCandidate> promise) override {
        UNREACHABLE();
      }

      Callback(td::actor::ActorId<TestNode> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorId<TestNode> id_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

  td::uint64 height_ = 0;

 public:
  void on_candidate(td::uint32 round, ton::PublicKeyHash source,
                    ton::validatorsession::ValidatorSessionRootHash root_hash, td::BufferSlice data,
                    td::BufferSlice collated,
                    td::Promise<ton::validatorsession::ValidatorSession::CandidateDecision> promise) {
    auto sh = sha256_bits256(data.as_slice());
    auto B = ton::fetch_tl_object<ton::ton_api::test_validatorSession_block>(std::move(data), true);
    if (B.is_error()) {
      promise.set_result(
          ton::validatorsession::ValidatorSession::CandidateDecision{B.move_as_error().to_string(), td::BufferSlice()});
      return;
    }
    if (collated.size() != 32) {
      promise.set_result(
          ton::validatorsession::ValidatorSession::CandidateDecision{"bad collated data length", td::BufferSlice()});
      return;
    }
    td::Bits256 x;
    x.as_slice().copy_from(collated.as_slice().truncate(32));
    if (x != sh) {
      promise.set_result(
          ton::validatorsession::ValidatorSession::CandidateDecision{"bad block hash", td::BufferSlice()});
      return;
    }
    auto block = B.move_as_ok();
    if (block->root_hash_ != root_hash) {
      promise.set_result(
          ton::validatorsession::ValidatorSession::CandidateDecision{"bad root hash", td::BufferSlice()});
      return;
    }
    if (block->root_hash_ != sha256_bits256(block->data_.as_slice())) {
      promise.set_result(
          ton::validatorsession::ValidatorSession::CandidateDecision{"bad root hash (2)", td::BufferSlice()});
      return;
    }
    if (block->height_ != static_cast<td::int64>(height_) + 1) {
      promise.set_result(
          ton::validatorsession::ValidatorSession::CandidateDecision{"bad root height", td::BufferSlice()});
      return;
    }
    promise.set_result(ton::validatorsession::ValidatorSession::CandidateDecision{0});
  }
  void on_generate_slot(td::uint32 round, td::Promise<ton::BlockCandidate> promise) {
    auto data = td::BufferSlice{10000};
    td::Random::secure_bytes(data.as_slice());
    auto root_hash = sha256_bits256(data.as_slice());
    auto block =
        ton::create_tl_object<ton::ton_api::test_validatorSession_block>(root_hash, height_ + 1, std::move(data));

    auto B = ton::serialize_tl_object(block, true);
    auto hash = sha256_bits256(B.as_slice());
    auto collated = td::BufferSlice{32};
    collated.as_slice().copy_from(as_slice(hash));

    /*BlockId id;
    BlockStatus status;
    RootHash root_hash;
    FileHash file_hash;
    FileHash collated_file_hash;
    td::BufferSlice data;
    td::BufferSlice collated_data;*/
    auto collated_file_hash = td::sha256_bits256(collated.as_slice());
    ton::BlockCandidate candidate{ton::BlockIdExt{ton::BlockId{0, 0, 0}, root_hash, td::sha256_bits256(B.as_slice())},
                                  collated_file_hash, std::move(B), std::move(collated)};
    promise.set_result(std::move(candidate));
  }
  void on_block_committed(td::uint32 round, ton::validatorsession::ValidatorSessionRootHash root_hash,
                          td::BufferSlice data,
                          std::vector<std::pair<ton::PublicKeyHash, td::BufferSlice>> signatures) {
    LOG(ERROR) << "COMITTED BLOCK: ROUND=" << round << " ROOT_HASH=" << root_hash
               << " DATA_HASH=" << sha256_bits256(data.as_slice()) << " SIGNED BY " << signatures.size();
  }
  void on_block_skipped(td::uint32 round) {
    LOG(ERROR) << "SKIPPED ROUND=" << round;
  }

  void set_local_config(std::string str) {
    local_config_ = str;
  }
  void set_global_config(std::string str) {
    global_config_ = str;
  }
  void start_up() override {
  }
  void alarm() override {
  }
  TestNode() {
  }
  void run() {
    keyring_ = ton::keyring::Keyring::create("/var/ton-work/db.keyring");
    adnl_ = ton::adnl::Adnl::create("/var/ton-work/db.adnl", keyring_.get());
    rldp_ = ton::rldp::Rldp::create(adnl_.get());

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
    //td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_ids_from_config, std::move(lc.local_ids_));
    if (gc.adnl_) {
      td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config,
                              std::move(gc.adnl_->static_nodes_));
    }
    if (!gc.dht_) {
      LOG(FATAL) << "global config does not contain dht section";
    }
    auto dhtR = ton::dht::Dht::create_global_config(std::move(gc.dht_));
    if (dhtR.is_error()) {
      LOG(FATAL) << "bad dht config: " << dhtR.move_as_error();
    }
    auto dht = dhtR.move_as_ok();

    for (auto &it : lc.dht_) {
      std::vector<ton::adnl::AdnlNodeIdShort> adnl_ids;
      ton::ton_api::downcast_call(
          *it.get(), td::overloaded(
                         [&](ton::ton_api::dht_config_local &obj) {
                           adnl_ids.push_back(ton::adnl::AdnlNodeIdShort{obj.id_->id_});
                         },
                         [&](ton::ton_api::dht_config_random_local &obj) {
                           auto addrR = ton::adnl::AdnlAddressList::create(std::move(obj.addr_list_));
                           addrR.ensure();
                           auto addr = addrR.move_as_ok();
                           for (td::int32 i = 0; i < obj.cnt_; i++) {
                             auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
                             auto pub = pk.compute_public_key();
                             td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false);
                             td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{pub},
                                                     addr);
                             auto adnl_id = ton::adnl::AdnlNodeIdShort{pub.compute_short_id()};
                             adnl_ids.push_back(adnl_id);
                           }
                         }));
      for (auto &id : adnl_ids) {
        auto R = ton::dht::Dht::create(id, "/var/ton-work/db/", dht, keyring_.get(), adnl_.get());
        R.ensure();
        dht_nodes_.push_back(R.move_as_ok());
      }
    }

    CHECK(dht_nodes_.size() > 0);

    td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_dht_node, dht_nodes_[0].get());
    //td::actor::send_closure(overlay_manager_, &ton::overlay::Overlays::register_dht_node, dht_nodes_[0].get());

    overlay_manager_ =
        ton::overlay::Overlays::create("/var/ton-work/db.overlays", keyring_.get(), adnl_.get(), dht_nodes_[0].get());

    //auto C = ton::CatChainActor::create(nullptr, adnl_.get(), overlay_manager_.get(),
    //                                    std::vector<ton::tl_object_ptr<ton::ton_api::adnl_id_Full>>());

    for (auto &it : lc.catchains_) {
      auto tag = it->tag_;
      for (auto &V : gc.catchains_) {
        if (V->tag_ == tag) {
          auto v = std::move(clone_tl_object(V)->nodes_);

          std::vector<ton::validatorsession::ValidatorSessionNode> w;
          w.resize(v.size());
          for (size_t i = 0; i < w.size(); i++) {
            w[i].pub_key = ton::PublicKey{v[i]};
            w[i].adnl_id = ton::adnl::AdnlNodeIdShort{w[i].pub_key.compute_short_id()};
            w[i].weight = 1;
          }

          auto C = ton::validatorsession::ValidatorSession::create(
              tag, ton::PublicKeyHash{it->id_->id_}, std::move(w), make_vs_callback(), keyring_.get(), adnl_.get(),
              rldp_.get(), overlay_manager_.get(), "/var/ton-work/db/");
          td::actor::send_closure(C, &ton::validatorsession::ValidatorSession::start);
          validator_sessions_.emplace_back(std::move(C));
        }
      }
    }
  }
};

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<TestNode> x;

  td::OptionParser p;
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
