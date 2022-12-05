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
#include "adnl/adnl-network-manager.h"
#include "adnl/adnl.h"
#include "adnl/utils.hpp"
#include "keys/encryptor.h"
#include "td/utils/Time.h"
#include "td/utils/format.h"
#include "td/utils/OptionParser.h"
#include "td/utils/filesystem.h"
#include "dht/dht.hpp"
#include "auto/tl/ton_api_json.h"
#include "common/delay.h"
#include "td/utils/Random.h"
#include "terminal/terminal.h"

#include <iostream>

class AdnlNode : public td::actor::Actor {
 private:
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager_;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;
  td::actor::ActorOwn<ton::keyring::Keyring> keyring_;
  ton::adnl::AdnlNodeIdShort local_id_;

  std::string host_ = "127.0.0.1";
  td::uint16 port_ = 2380;

  std::string global_config_ = "ton-global.config";

  struct NodeInfo {
    ton::adnl::AdnlNodeIdShort id;
    td::uint32 sent = 0, received = 0;
    double sum_time = 0.0;
    explicit NodeInfo(ton::adnl::AdnlNodeIdShort id) : id(id) {
    }
  };
  std::vector<NodeInfo> nodes_;

  td::uint32 pings_remaining_ = 4;
  td::uint32 pending_ = 1;

 public:
  void set_global_config(std::string str) {
    global_config_ = str;
  }
  void listen_udp(td::uint16 port) {
    port_ = port;
  }

  AdnlNode() {
  }

  void run() {
    network_manager_ = ton::adnl::AdnlNetworkManager::create(port_);
    keyring_ = ton::keyring::Keyring::create("");
    adnl_ = ton::adnl::Adnl::create("", keyring_.get());
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_network_manager, network_manager_.get());

    td::IPAddress addr;
    addr.init_host_port(host_, port_).ensure();
    ton::adnl::AdnlCategoryMask mask;
    mask[0] = true;
    td::actor::send_closure(network_manager_, &ton::adnl::AdnlNetworkManager::add_self_addr, addr, mask, 0);
    auto pk = ton::privkeys::Ed25519::random();
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, pk, true, [](td::Result<td::Unit>) {});
    ton::adnl::AdnlNodeIdFull local_id_full(pk.pub());
    ton::adnl::AdnlAddressList addr_list;
    addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
    addr_list.set_reinit_date(ton::adnl::Adnl::adnl_start_time());
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, local_id_full, std::move(addr_list), (td::uint8)0);
    local_id_ = local_id_full.compute_short_id();

    auto r_dht = get_dht_config();
    if (r_dht.is_error()) {
      LOG(FATAL) << "Cannot get dht config: " << r_dht.move_as_error();
    }
    auto dht = r_dht.move_as_ok();
    ton::adnl::AdnlNodesList static_nodes;
    for (const auto &node : dht->nodes().list()) {
      LOG(INFO) << "Node #" << nodes_.size() << " : " << node.adnl_id().compute_short_id();
      nodes_.emplace_back(node.adnl_id().compute_short_id());
      static_nodes.push(ton::adnl::AdnlNode(node.adnl_id(), node.addr_list()));
    }
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config, std::move(static_nodes));

    ton::delay_action([SelfId = actor_id(this)]() { td::actor::send_closure(SelfId, &AdnlNode::send_pings); },
                      td::Timestamp::in(1.0));
  }

  td::Result<std::shared_ptr<ton::dht::DhtGlobalConfig>> get_dht_config() {
    TRY_RESULT_PREFIX(conf_data, td::read_file(global_config_), "failed to read: ");
    TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");
    ton::ton_api::config_global conf;
    TRY_STATUS_PREFIX(ton::ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");
    if (!conf.dht_) {
      return td::Status::Error(ton::ErrorCode::error, "does not contain [dht] section");
    }
    TRY_RESULT_PREFIX(dht, ton::dht::Dht::create_global_config(std::move(conf.dht_)), "bad [dht] section: ");
    return std::move(dht);
  }

  void send_pings() {
    CHECK(pings_remaining_);
    --pings_remaining_;
    for (size_t i = 0; i < nodes_.size(); ++i) {
      auto id = nodes_[i].id;
      LOG(INFO) << "Sending ping to " << id;
      ++pending_;
      td::actor::send_closure(
          adnl_, &ton::adnl::Adnl::send_query, local_id_, id, "ping",
          [SelfId = actor_id(this), i, timer = td::Timer()](td::Result<td::BufferSlice> R) {
            td::actor::send_closure(SelfId, &AdnlNode::on_pong, i, timer.elapsed(), R.is_ok());
          }, td::Timestamp::in(5.0),
          ton::create_serialize_tl_object<ton::ton_api::dht_ping>(td::Random::fast_uint64()));
    }

    if (pings_remaining_ == 0) {
      --pending_;
      try_finish();
    } else {
      ton::delay_action([SelfId = actor_id(this)]() { td::actor::send_closure(SelfId, &AdnlNode::send_pings); },
                        td::Timestamp::in(1.0));
    }
  }

  void on_pong(size_t i, double time, bool success) {
    auto &node = nodes_[i];
    ++node.sent;
    if (success) {
      ++node.received;
      node.sum_time += time;
      LOG(INFO) << "Pong from " << node.id << " in " << time << "s";
    } else {
      LOG(INFO) << "Pong from " << node.id << " : timeout";
    }
    --pending_;
    try_finish();
  }

  void try_finish() {
    if (pending_) {
      return;
    }
    td::TerminalIO::out() << "Pinged " << nodes_.size() << " nodes:\n";
    for (const auto& node : nodes_) {
      td::TerminalIO::out() << node.id << " : " << node.received << "/" << node.sent;
      if (node.received > 0) {
        td::TerminalIO::out() << " (avg. time = " << node.sum_time / node.received << ")";
      }
      td::TerminalIO::out() << "\n";
    }
    std::exit(0);
  }
};

int main(int argc, char *argv[]) {
  td::actor::ActorOwn<AdnlNode> x;

  td::OptionParser p;
  p.set_description("ping dht servers from config");
  p.add_option('h', "help", "print help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('p', "port", "set udp port", [&](td::Slice port) {
    td::actor::send_closure(x, &AdnlNode::listen_udp, static_cast<td::uint16>(std::stoi(port.str())));
  });
  p.add_option('C', "global-config", "file to read global config from",
               [&](td::Slice fname) { td::actor::send_closure(x, &AdnlNode::set_global_config, fname.str()); });
  p.add_option('v', "verbosity", "set verbosity", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });

  td::actor::Scheduler scheduler({2});

  scheduler.run_in_context([&] { x = td::actor::create_actor<AdnlNode>("AdnlNode"); });

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] { td::actor::send_closure(x, &AdnlNode::run); });

  scheduler.run();

  return 0;
}
