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

#include <iostream>
#include <sstream>

template <std::size_t size>
std::ostream &operator<<(std::ostream &stream, const td::UInt<size> &x) {
  for (size_t i = 0; i < size / 8; i++) {
    stream << td::format::hex_digit((x.raw[i] >> 4) & 15) << td::format::hex_digit(x.raw[i] & 15);
  }

  return stream;
}

class AdnlNode : public td::actor::Actor {
 private:
  std::vector<td::UInt256> ping_ids_;

  td::actor::ActorOwn<ton::AdnlNetworkManager> network_manager_;
  td::actor::ActorOwn<ton::AdnlPeerTable> peer_table_;

  td::UInt256 local_id_;
  bool local_id_set_ = false;

  std::string host_ = "127.0.0.1";
  td::uint32 ip_ = 0x7f000001;
  td::uint16 port_ = 2380;

  void receive_message(td::UInt256 src, td::UInt256 dst, td::BufferSlice data) {
    std::cout << "MESSAGE FROM " << src << " to " << dst << " of size " << std::to_string(data.size()) << "\n";
  }

  void receive_query(td::UInt256 src, td::UInt256 dst, td::uint64 query_id, td::BufferSlice data) {
    std::cout << "QUERY " << std::to_string(query_id) << " FROM " << src << " to " << dst << " of size "
              << std::to_string(data.size()) << "\n";
    td::actor::send_closure(peer_table_, &ton::AdnlPeerTable::answer_query, dst, src, query_id,
                            ton::create_tl_object<ton::ton_api::testObject>());
  }

  std::unique_ptr<ton::AdnlPeerTable::Callback> make_callback() {
    class Callback : public ton::AdnlPeerTable::Callback {
     public:
      void receive_message(td::UInt256 src, td::UInt256 dst, td::BufferSlice data) override {
        td::actor::send_closure(id_, &AdnlNode::receive_message, src, dst, std::move(data));
      }
      void receive_query(td::UInt256 src, td::UInt256 dst, td::uint64 query_id, td::BufferSlice data) override {
        td::actor::send_closure(id_, &AdnlNode::receive_query, src, dst, query_id, std::move(data));
      }
      Callback(td::actor::ActorId<AdnlNode> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorId<AdnlNode> id_;
    };

    return std::make_unique<Callback>(td::actor::actor_id(this));
  }

 public:
  void start_up() override {
    alarm_timestamp() = td::Timestamp::in(1);
  }
  AdnlNode() {
    network_manager_ = ton::AdnlNetworkManager::create();
    peer_table_ = ton::AdnlPeerTable::create();
    td::actor::send_closure(network_manager_, &ton::AdnlNetworkManager::register_peer_table, peer_table_.get());
    td::actor::send_closure(peer_table_, &ton::AdnlPeerTable::register_network_manager, network_manager_.get());
  }
  void listen_udp(td::uint16 port) {
    td::actor::send_closure(network_manager_, &ton::AdnlNetworkManager::add_listening_udp_port, "0.0.0.0", port);
    port_ = port;
  }
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
    td::actor::send_closure(peer_table_, &ton::AdnlPeerTable::add_id, std::move(pk_), std::move(y));
    td::actor::send_closure(peer_table_, &ton::AdnlPeerTable::subscribe, local_id_, "", make_callback());
    local_id_set_ = true;
  }

  void add_foreign(ton::tl_object_ptr<ton::ton_api::adnl_id_Full> id,
                   ton::tl_object_ptr<ton::ton_api::adnl_addressList> addr_list) {
    std::cout << ton::adnl_short_id(id) << "\n";
    td::actor::send_closure(peer_table_, &ton::AdnlPeerTable::add_peer, std::move(id), std::move(addr_list));
  }

  void alarm() override {
    std::cout << "alarm\n";
    if (local_id_set_) {
      for (auto it = ping_ids_.begin(); it != ping_ids_.end(); it++) {
        auto P = td::PromiseCreator::lambda([](td::Result<td::BufferSlice> result) {
          if (result.is_error()) {
            std::cout << "received error " << result.move_as_error().to_string() << "\n";
          } else {
            auto message = result.move_as_ok();
            std::cout << "received answer to query\n";
          }
        });
        td::actor::send_closure(peer_table_, &ton::AdnlPeerTable::send_query, local_id_, *it, std::move(P),
                                td::Timestamp::in(5), ton::create_tl_object<ton::ton_api::getTestObject>());
      }
    }

    alarm_timestamp() = td::Timestamp::in(1);
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
  td::actor::ActorOwn<AdnlNode> x;

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
    td::actor::send_closure(x, &AdnlNode::listen_udp, static_cast<td::uint16>(std::stoi(port.str())));
    return td::Status::OK();
  });
  p.add_option('a', "host", "sets local ip", [&](td::Slice ip) {
    td::IPAddress addr;
    auto R = addr.init_host_port(ip.str(), 0);
    if (R.is_error()) {
      return R;
    }
    td::actor::send_closure(x, &AdnlNode::set_host, addr, ip.str());
    return td::Status::OK();
  });
  p.add_option('i', "id", "sets local id", [&](td::Slice id) {
    td::actor::send_closure(x, &AdnlNode::add_local_id,
                            ton::create_tl_object<ton::ton_api::adnl_id_pk_unenc>(id.str()));
    return td::Status::OK();
  });
  p.add_option('P', "peer", "adds peer id@host:port", [&](td::Slice id) {
    auto pos = id.rfind('@');
    if (pos == static_cast<size_t>(-1)) {
      return td::Status::Error("--peer expected randomtag@host:port as argument");
    }
    auto s1 = id.substr(0, pos);
    auto f_id = ton::create_tl_object<ton::ton_api::adnl_id_unenc>(s1.str());
    td::IPAddress addr;
    auto R = addr.init_host_port(td::CSlice(id.substr(pos + 1).str()));
    if (R.is_error()) {
      return R.move_as_error();
    }

    auto f_addr = ton::create_tl_object<ton::ton_api::adnl_address_udp>(addr.get_ipv4(), addr.get_port());
    std::vector<ton::tl_object_ptr<ton::ton_api::adnl_Address>> vv;
    vv.push_back(ton::move_tl_object_as<ton::ton_api::adnl_Address>(f_addr));

    auto f_addr_list =
        ton::create_tl_object<ton::ton_api::adnl_addressList>(std::move(vv), static_cast<int>(td::Time::now()));

    td::actor::send_closure(x, &AdnlNode::add_foreign, ton::move_tl_object_as<ton::ton_api::adnl_id_Full>(f_id),
                            std::move(f_addr_list));

    return td::Status::OK();
  });
  p.add_option('n', "node", "node to send pings to", [&](td::Slice node) {
    auto R = get_uint256(node.str());
    if (R.is_error()) {
      return R.move_as_error();
    }

    td::actor::send_closure(x, &AdnlNode::send_pings_to, R.move_as_ok());
    return td::Status::OK();
  });

  td::actor::Scheduler scheduler({2});
  scheduler.run_in_context([&] {
    x = td::actor::create_actor<AdnlNode>(td::actor::ActorInfoCreator::Options().with_name("A").with_poll());
  });
  scheudler.run();
  return 0;
}
