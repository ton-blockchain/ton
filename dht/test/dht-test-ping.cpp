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
#include "adnl/adnl-peer-table.h"
#include "adnl/utils.hpp"
#include "keys/encryptor.h"
#include "td/utils/Time.h"
#include "td/utils/format.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/filesystem.h"
#include "dht/dht.h"
#include "auto/tl/ton_api_json.h"

#include <iostream>
#include <sstream>

template <std::size_t size>
std::ostream &operator<<(std::ostream &stream, const td::UInt<size> &x) {
  for (size_t i = 0; i < size / 8; i++) {
    stream << td::format::hex_digit((x.raw[i] >> 4) & 15) << td::format::hex_digit(x.raw[i] & 15);
  }

  return stream;
}

class adnl::AdnlNode : public td::actor::Actor {
 private:
  std::vector<td::UInt256> ping_ids_;

  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager_;
  td::actor::ActorOwn<ton::adnl::AdnlPeerTable> peer_table_;
  td::actor::ActorOwn<ton::DhtNode> dht_node_;

  td::UInt256 local_id_;
  bool local_id_set_ = false;

  std::string host_ = "127.0.0.1";
  td::uint32 ip_ = 0x7f000001;
  td::uint16 port_ = 2380;

  std::string local_config_ = "ton-local.config";
  std::string global_config_ = "ton-global.config";

  void receive_message(td::UInt256 src, td::UInt256 dst, td::BufferSlice data) {
    std::cout << "MESSAGE FROM " << src << " to " << dst << " of size " << std::to_string(data.size()) << "\n";
  }

  void receive_query(td::UInt256 src, td::UInt256 dst, td::uint64 query_id, td::BufferSlice data) {
    std::cout << "QUERY " << std::to_string(query_id) << " FROM " << src << " to " << dst << " of size "
              << std::to_string(data.size()) << "\n";
    td::actor::send_closure(peer_table_, &ton::adnl::AdnlPeerTable::answer_query, dst, src, query_id,
                            ton::create_tl_object<ton::ton_api::testObject>());
  }

  std::unique_ptr<ton::adnl::AdnlPeerTable::Callback> make_callback() {
    class Callback : public ton::adnl::AdnlPeerTable::Callback {
     public:
      void receive_message(td::UInt256 src, td::UInt256 dst, td::BufferSlice data) override {
        td::actor::send_closure(id_, &adnl::AdnlNode::receive_message, src, dst, std::move(data));
      }
      void receive_query(td::UInt256 src, td::UInt256 dst, td::uint64 query_id, td::BufferSlice data) override {
        td::actor::send_closure(id_, &adnl::AdnlNode::receive_query, src, dst, query_id, std::move(data));
      }
      Callback(td::actor::ActorId<adnl::AdnlNode> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorId<adnl::AdnlNode> id_;
    };

    return std::make_unique<Callback>(td::actor::actor_id(this));
  }

 public:
  void set_local_config(std::string str) {
    local_config_ = str;
  }
  void set_global_config(std::string str) {
    global_config_ = str;
  }
  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(1);
  }
  adnl::AdnlNode() {
    network_manager_ = ton::adnl::AdnlNetworkManager::create();
    peer_table_ = ton::adnl::AdnlPeerTable::create();
    td::actor::send_closure(network_manager_, &ton::adnl::AdnlNetworkManager::register_peer_table, peer_table_.get());
    td::actor::send_closure(peer_table_, &ton::adnl::AdnlPeerTable::register_network_manager, network_manager_.get());
  }
  void listen_udp(td::uint16 port) {
    td::actor::send_closure(network_manager_, &ton::adnl::AdnlNetworkManager::add_listening_udp_port, "0.0.0.0", port);
    port_ = port;
  }
  void run() {
    auto L = td::read_file(local_config_);
    if (L.is_error()) {
      LOG(FATAL) << "can not read local config: " << L.move_as_error();
    }
    auto L2 = td::json_decode(L.move_as_ok().as_slice());
    if (L2.is_error()) {
      LOG(FATAL) << "can not parse local config: " << L2.move_as_error();
    }
    auto lc_j = L2.move_as_ok();
    if (lc_j.type() != td::JsonValue::Type::Object) {
      LOG(FATAL) << "can not parse local config: expected json object";
    }

    ton::ton_api::config_local lc;
    auto rl = ton::ton_api::from_json(lc, lc_j.get_object());
    if (rl.is_error()) {
      LOG(FATAL) << "can not interpret local config: " << rl.move_as_error();
    }

    auto G = td::read_file(global_config_);
    if (G.is_error()) {
      LOG(FATAL) << "can not read global config: " << G.move_as_error();
    }
    auto G2 = td::json_decode(G.move_as_ok().as_slice());
    if (G2.is_error()) {
      LOG(FATAL) << "can not parse global config: " << G2.move_as_error();
    }
    auto gc_j = G2.move_as_ok();
    if (gc_j.type() != td::JsonValue::Type::Object) {
      LOG(FATAL) << "can not parse global config: expected json object";
    }

    ton::ton_api::config_global gc;
    auto rg = ton::ton_api::from_json(gc, gc_j.get_object());
    if (rg.is_error()) {
      LOG(FATAL) << "can not interpret local config: " << rg.move_as_error();
    }

    if (gc.adnl_) {
      auto it = gc.adnl_->static_nodes_.begin();
      while (it != gc.adnl_->static_nodes_.end()) {
        auto R = ton::adnl_validate_full_id(std::move((*it)->id_));
        if (R.is_error()) {
          LOG(FATAL) << "can not apply global config: " << R.move_as_error();
        }
        auto R2 = ton::adnl_validate_addr_list(std::move((*it)->addr_list_));
        if (R2.is_error()) {
          LOG(FATAL) << "can not apply global config: " << R2.move_as_error();
        }
        td::actor::send_closure(peer_table_, &ton::adnl::AdnlPeerTable::add_peer, R.move_as_ok(), R2.move_as_ok());
        it++;
      }
    }

    if (!gc.dht_) {
      LOG(FATAL) << "global config does not contain dht section";
    }
    if (lc.dht_.size() != 1) {
      LOG(FATAL) << "local config must contain exactly one dht section";
    }

    auto R = ton::DhtNode::create_from_json(std::move(gc.dht_), std::move(lc.dht_[0]), peer_table_.get());
    if (R.is_error()) {
      LOG(FATAL) << "fail creating dht node: " << R.move_as_error();
    }

    dht_node_ = R.move_as_ok();
  }
  /*
  void set_host(td::IPAddress ip, std::string host) {
    ip_ = ip.get_ipv4();
    host_ = host;
  }
  void send_pings_to(td::UInt256 id) {
    std::cout << "send pings to " << id << "\n";
    ping_ids_.push_back(id);
  }
  void add_local_id(ton::tl_object_ptr<ton::ton_api::adnl_id_Pk> pk_) {
    auto pub_ = ton::get_public_key(pk_);
    local_id_ = ton::adnl_short_id(pub_);
    std::cout << "local_id = '" << local_id_ << "'\n";
    auto x = ton::create_tl_object<ton::ton_api::adnl_address_udp>(ip_, port_);
    auto v = std::vector<ton::tl_object_ptr<ton::ton_api::adnl_Address>>();
    v.push_back(ton::move_tl_object_as<ton::ton_api::adnl_Address>(x));
    auto y =
        ton::create_tl_object<ton::ton_api::adnl_addressList>(std::move(v), static_cast<td::int32>(td::Time::now()));

    LOG(INFO) << "local_addr_list: " << ton::ton_api::to_string(y);
    td::actor::send_closure(peer_table_, &ton::adnl::AdnlPeerTable::add_id, ton::clone_tl_object(pk_),
                             ton::clone_tl_object(y));
    td::actor::send_closure(peer_table_, &ton::adnl::AdnlPeerTable::subscribe_custom, local_id_, "TEST", make_callback());
    local_id_set_ = true;

    dht_node_ = ton::DhtNode::create(std::move(pk_), peer_table_.get());
    td::actor::send_closure(dht_node_, &ton::DhtNode::update_addr_list, std::move(y));
  }

  void add_static_dht_node(ton::tl_object_ptr<ton::ton_api::adnl_id_Full> id,
                           ton::tl_object_ptr<ton::ton_api::adnl_addressList> addr_list,
                           td::BufferSlice signature) {
    auto Id = ton::adnl_short_id(id);
    td::actor::send_closure(
        dht_node_, &ton::DhtNode::add_full_node, Id,
        ton::create_tl_object<ton::ton_api::dht_node>(std::move(id), std::move(addr_list), signature.as_slice().str()));
  }

  void add_foreign(ton::tl_object_ptr<ton::ton_api::adnl_id_Full> id,
                   ton::tl_object_ptr<ton::ton_api::adnl_addressList> addr_list) {
    std::cout << ton::adnl_short_id(id) << "\n";
    td::actor::send_closure(peer_table_, &ton::adnl::AdnlPeerTable::add_peer, std::move(id), std::move(addr_list));
  }

  void alarm() override {
    std::cout << "alarm\n";
    if (local_id_set_) {
      for (auto it = ping_ids_.begin(); it != ping_ids_.end(); it++) {
        auto P = td::PromiseCreator::lambda([](td::Result<ton::tl_object_ptr<ton::ton_api::adnl_Message>> result) {
          if (result.is_error()) {
            std::cout << "received error " << result.move_as_error().to_string() << "\n";
          } else {
            auto message = result.move_as_ok();
            std::cout << "received answer to query\n";
          }
        });
        td::actor::send_closure(peer_table_, &ton::adnl::AdnlPeerTable::send_query, local_id_, *it, std::move(P),
                                 td::Timestamp::in(5),
                                 ton::move_tl_object_as<ton::ton_api::adnl_Message>(
                                     ton::create_tl_object<ton::ton_api::adnl_message_custom>("TEST")));
      }
    }
  }
  */
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
  td::actor::ActorOwn<adnl::AdnlNode> x;

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
  p.add_option('p', "port", "sets udp port", [&](td::Slice port) {
    td::actor::send_closure(x, &adnl::AdnlNode::listen_udp, static_cast<td::uint16>(std::stoi(port.str())));
    return td::Status::OK();
  });
  p.add_option('C', "global-config", "file to read global config", [&](td::Slice fname) {
    td::actor::send_closure(x, &adnl::AdnlNode::set_global_config, fname.str());
    return td::Status::OK();
  });
  p.add_option('c', "local-config", "file to read local config", [&](td::Slice fname) {
    td::actor::send_closure(x, &adnl::AdnlNode::set_local_config, fname.str());
    return td::Status::OK();
  });

  td::actor::Scheduler scheduler({2});

  scheduler.run_in_context([&] {
    x = td::actor::create_actor<adnl::AdnlNode>(td::actor::ActorInfoCreator::Options().with_name("A").with_poll());
  });

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] { td::actor::send_closure(x, &adnl::AdnlNode::run); });

  scheduler.run();

  return 0;
}
