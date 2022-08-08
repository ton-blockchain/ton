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
#include "adnl/adnl-test-loopback-implementation.h"
#include "dht/dht.h"
#include "overlay/overlays.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/port/path.h"
#include "td/utils/Random.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/FileFd.h"
#include "td/utils/overloaded.h"
#include "catchain/catchain.h"
#include "common/errorlog.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <iostream>
#include <sstream>

#include <set>

struct Node {
  ton::PublicKeyHash id;
  ton::PublicKey id_full;
  ton::adnl::AdnlNodeIdShort adnl_id;
  ton::adnl::AdnlNodeIdFull adnl_id_full;
};

class CatChainInst : public td::actor::Actor {
 public:
  class PayloadExtra : public ton::catchain::CatChainBlock::Extra {
   public:
    PayloadExtra(td::uint64 sum) : sum(sum) {
    }
    td::uint64 sum;
  };
  void process_blocks(std::vector<ton::catchain::CatChainBlock *> blocks) {
    td::uint64 sum = sum_;
    for (auto &B : blocks) {
      auto E = dynamic_cast<const PayloadExtra *>(B->extra());
      CHECK(E);
      sum = std::max(sum, E->sum);
    }
    td::uint64 value = td::Random::fast_uint64();
    sum = std::max(sum, value);
    td::uint64 x[2];
    x[0] = value;
    x[1] = sum;

    sum_ = sum;

    td::actor::send_closure(catchain_, &ton::catchain::CatChain::processed_block,
                            td::BufferSlice{td::Slice{reinterpret_cast<char *>(x), 16}});

    alarm_timestamp() = td::Timestamp::in(0.1);
    height_++;
    prev_values_.push_back(sum_);
  }
  void finished_processing() {
  }
  void preprocess_block(ton::catchain::CatChainBlock *block) {
    td::uint64 sum = 0;
    auto prev = block->prev();
    if (prev) {
      auto E = dynamic_cast<const PayloadExtra *>(prev->extra());
      CHECK(E);
      sum = std::max(E->sum, sum);
    }
    for (auto &B : block->deps()) {
      auto E = dynamic_cast<const PayloadExtra *>(B->extra());
      CHECK(E);
      sum = std::max(E->sum, sum);
    }

    auto &payload = block->payload();
    if (!payload.empty()) {
      CHECK(payload.size() == 16);
      td::uint64 x[2];
      td::MutableSlice{reinterpret_cast<td::uint8 *>(x), 16}.copy_from(payload.as_slice());
      sum = std::max(sum, x[0]);
      LOG_CHECK(sum == x[1]) << sum << " " << x[0];
    } else {
      CHECK(!block->deps().size());
    }
    block->set_extra(std::make_unique<PayloadExtra>(sum));
  }

  void alarm() override {
    td::actor::send_closure(catchain_, &ton::catchain::CatChain::need_new_block, td::Timestamp::in(0.1));
  }

  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(0.1);
    ton::CatChainOptions opts;
    opts.debug_disable_db = true;
    //opts.block_hash_covers_data = true;

    std::vector<ton::catchain::CatChainNode> nodes;
    for (auto &n : nodes_) {
      nodes.push_back(ton::catchain::CatChainNode{n.adnl_id, n.id_full});
    }
    catchain_ =
        ton::catchain::CatChain::create(make_callback(), opts, keyring_, adnl_, overlay_manager_, std::move(nodes),
                                        nodes_[idx_].id, unique_hash_, std::string(""), "", false);
  }

  CatChainInst(td::actor::ActorId<ton::keyring::Keyring> keyring, td::actor::ActorId<ton::adnl::Adnl> adnl,
               td::actor::ActorId<ton::overlay::Overlays> overlay_manager, std::vector<Node> nodes, td::uint32 idx,
               ton::catchain::CatChainSessionId unique_hash)
      : keyring_(keyring)
      , adnl_(adnl)
      , overlay_manager_(overlay_manager)
      , nodes_(std::move(nodes))
      , idx_(idx)
      , unique_hash_(unique_hash) {
  }

  std::unique_ptr<ton::catchain::CatChain::Callback> make_callback() {
    class Callback : public ton::catchain::CatChain::Callback {
     public:
      void process_blocks(std::vector<ton::catchain::CatChainBlock *> blocks) override {
        td::actor::send_closure(id_, &CatChainInst::process_blocks, std::move(blocks));
      }
      void finished_processing() override {
        td::actor::send_closure(id_, &CatChainInst::finished_processing);
      }
      void preprocess_block(ton::catchain::CatChainBlock *block) override {
        td::actor::send_closure(id_, &CatChainInst::preprocess_block, std::move(block));
      }
      void process_broadcast(const ton::PublicKeyHash &src, td::BufferSlice data) override {
        UNREACHABLE();
      }
      void process_message(const ton::PublicKeyHash &src, td::BufferSlice data) override {
        UNREACHABLE();
      }
      void process_query(const ton::PublicKeyHash &src, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        UNREACHABLE();
      }
      void started() override {
      }
      Callback(td::actor::ActorId<CatChainInst> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorId<CatChainInst> id_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

  td::uint64 value() {
    return sum_;
  }

  void create_fork() {
    auto height = height_ - 1;  //td::Random::fast(0, height_ - 1);

    auto sum = prev_values_[height] + 1;
    td::uint64 x[2];
    x[0] = sum + 1;
    x[1] = sum + 1;

    td::actor::send_closure(catchain_, &ton::catchain::CatChain::debug_add_fork,
                            td::BufferSlice{td::Slice{reinterpret_cast<char *>(x), 16}}, height + 1);
  }

 private:
  td::actor::ActorId<ton::keyring::Keyring> keyring_;
  td::actor::ActorId<ton::adnl::Adnl> adnl_;
  td::actor::ActorId<ton::overlay::Overlays> overlay_manager_;

  std::vector<Node> nodes_;
  td::uint32 idx_;

  ton::catchain::CatChainSessionId unique_hash_;

  td::actor::ActorOwn<ton::catchain::CatChain> catchain_;
  td::uint64 sum_ = 0;
  td::uint32 height_ = 0;
  std::vector<td::uint64> prev_values_;
};

static std::vector<Node> nodes;
static td::uint32 total_nodes = 11;

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  std::string db_root_ = "tmp-ee";
  td::rmrf(db_root_).ignore();
  td::mkdir(db_root_).ensure();

  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<ton::keyring::Keyring> keyring;
  td::actor::ActorOwn<ton::adnl::TestLoopbackNetworkManager> network_manager;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl;
  td::actor::ActorOwn<ton::overlay::Overlays> overlay_manager;

  td::actor::Scheduler scheduler({7});
  scheduler.run_in_context([&] {
    ton::errorlog::ErrorLog::create(db_root_);
    keyring = ton::keyring::Keyring::create(db_root_);
    network_manager = td::actor::create_actor<ton::adnl::TestLoopbackNetworkManager>("test net");
    adnl = ton::adnl::Adnl::create(db_root_, keyring.get());
    overlay_manager =
        ton::overlay::Overlays::create(db_root_, keyring.get(), adnl.get(), td::actor::ActorId<ton::dht::Dht>{});
    td::actor::send_closure(adnl, &ton::adnl::Adnl::register_network_manager, network_manager.get());
  });

  for (td::uint32 att = 0; att < 10; att++) {
    nodes.resize(total_nodes);

    scheduler.run_in_context([&] {
      auto addr = ton::adnl::TestLoopbackNetworkManager::generate_dummy_addr_list();

      for (auto &n : nodes) {
        auto pk1 = ton::PrivateKey{ton::privkeys::Ed25519::random()};
        auto pub1 = pk1.compute_public_key();
        n.adnl_id_full = ton::adnl::AdnlNodeIdFull{pub1};
        n.adnl_id = ton::adnl::AdnlNodeIdShort{pub1.compute_short_id()};
        td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, std::move(pk1), true, [](td::Unit) {});
        td::actor::send_closure(adnl, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{pub1}, addr,
                                static_cast<td::uint8>(0));
        td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, n.adnl_id, true,
                                true);

        auto pk2 = ton::PrivateKey{ton::privkeys::Ed25519::random()};
        auto pub2 = pk2.compute_public_key();
        n.id_full = pub2;
        n.id = pub2.compute_short_id();
        td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, std::move(pk2), true, [](td::Unit) {});

        LOG(DEBUG) << "created node " << n.adnl_id << " " << n.id;
      }

      for (auto &n1 : nodes) {
        for (auto &n2 : nodes) {
          td::actor::send_closure(adnl, &ton::adnl::Adnl::add_peer, n1.adnl_id, n2.adnl_id_full, addr);
        }
      }
    });

    auto t = td::Timestamp::in(1.0);

    ton::catchain::CatChainSessionId unique_id;
    td::Random::secure_bytes(unique_id.as_slice());

    std::vector<td::actor::ActorOwn<CatChainInst>> inst;
    scheduler.run_in_context([&] {
      for (td::uint32 idx = 0; idx < total_nodes; idx++) {
        inst.push_back(td::actor::create_actor<CatChainInst>("inst", keyring.get(), adnl.get(), overlay_manager.get(),
                                                             nodes, idx, unique_id));
      }
    });

    t = td::Timestamp::in(10.0);
    while (scheduler.run(1)) {
      if (t.is_in_past()) {
        break;
      }
    }

    for (auto &n : inst) {
      std::cout << "value=" << n.get_actor_unsafe().value() << std::endl;
    }

    scheduler.run_in_context([&] { td::actor::send_closure(inst[0], &CatChainInst::create_fork); });

    t = td::Timestamp::in(10.0);
    while (scheduler.run(1)) {
      if (t.is_in_past()) {
        break;
      }
    }

    for (auto &n : inst) {
      std::cout << "value=" << n.get_actor_unsafe().value() << std::endl;
    }

    scheduler.run_in_context([&] {
      nodes.clear();
      inst.clear();
    });
  }

  td::rmrf(db_root_).ensure();
  std::_Exit(0);
  return 0;
}
