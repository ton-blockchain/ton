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
#include "common/util.h"

#include <iostream>

class Resolver : public td::actor::Actor {
 private:
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager_;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;
  td::actor::ActorOwn<ton::keyring::Keyring> keyring_;
  ton::adnl::AdnlNodeIdShort local_id_;
  td::actor::ActorOwn<ton::dht::Dht> dht_;

  std::string global_config_;
  int server_idx_;

  std::string host_ = "127.0.0.1";
  td::uint16 port_;
  ton::dht::DhtKey key_;
  double timeout_;

 public:
  Resolver(std::string global_config, int server_idx, td::uint16 port, ton::dht::DhtKey key, double timeout)
      : global_config_(global_config), server_idx_(server_idx), port_(port), key_(std::move(key)), timeout_(timeout) {
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

    auto dht_config = get_dht_config();
    if (dht_config.is_error()) {
      LOG(FATAL) << "Failed to load dht config: " << dht_config.move_as_error();
    }
    auto D = ton::dht::Dht::create_client(local_id_, "", dht_config.move_as_ok(), keyring_.get(), adnl_.get());
    if (D.is_error()) {
      LOG(FATAL) << "Failed to init dht client: " << D.move_as_error();
    }
    dht_ = D.move_as_ok();
    LOG(INFO) << "Get value " << key_.public_key_hash() << " " << key_.name() << " " << key_.idx();

    send_query();
    alarm_timestamp() = td::Timestamp::in(timeout_);
  }

  void send_query() {
    td::actor::send_closure(dht_, &ton::dht::Dht::get_value, key_,
                            [SelfId = actor_id(this)](td::Result<ton::dht::DhtValue> R) {
                              td::actor::send_closure(SelfId, &Resolver::got_result, std::move(R));
                            });
  }

  void got_result(td::Result<ton::dht::DhtValue> R) {
    if (R.is_error()) {
      LOG(WARNING) << "Failed to get value, retrying: " << R.move_as_error();
      ton::delay_action([SelfId = actor_id(this)]() { td::actor::send_closure(SelfId, &Resolver::send_query); },
                        td::Timestamp::in(0.25));
      return;
    }
    auto r = R.move_as_ok();
    LOG(INFO) << "Got result";
    td::TerminalIO::out() << "KEY: " << td::base64_encode(ton::serialize_tl_object(r.key().public_key().tl(), true))
                          << "\n";
    td::TerminalIO::out() << "VALUE: " << td::base64_encode(r.value().as_slice()) << "\n";
    std::exit(0);
  }

  void alarm() override {
    LOG(FATAL) << "Failed to get value: timeout";
  }

  td::Result<std::shared_ptr<ton::dht::DhtGlobalConfig>> get_dht_config() {
    TRY_RESULT_PREFIX(conf_data, td::read_file(global_config_), "failed to read: ");
    TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");
    ton::ton_api::config_global conf;
    TRY_STATUS_PREFIX(ton::ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");
    if (!conf.dht_) {
      return td::Status::Error(ton::ErrorCode::error, "does not contain [dht] section");
    }
    ton::ton_api::dht_nodes* static_nodes = nullptr;
    ton::ton_api::downcast_call(*conf.dht_, [&](auto &f) { static_nodes = f.static_nodes_.get(); });
    auto &nodes = static_nodes->nodes_;
    if (server_idx_ >= 0) {
      CHECK(server_idx_ < (int)nodes.size());
      LOG(INFO) << "Using server #" << server_idx_;
      std::swap(nodes[0], nodes[server_idx_]);
      nodes.resize(1);
    } else {
      LOG(INFO) << "Using all " << nodes.size() << " servers";
    }
    TRY_RESULT_PREFIX(dht, ton::dht::Dht::create_global_config(std::move(conf.dht_)), "bad [dht] section: ");
    return std::move(dht);
  }
};

td::Result<td::Bits256> parse_bits256(td::Slice s) {
  td::BufferSlice str = td::base64_decode(s, true);
  if (str.size() != 32) {
    return td::Status::Error("Invalid bits256");
  }
  return td::Bits256(td::BitPtr((unsigned char *)str.data()));
}

int main(int argc, char *argv[]) {
  td::actor::ActorOwn<Resolver> x;

  td::optional<std::string> global_config;
  int server_idx = -1;
  td::uint16 port = 2380;
  td::optional<td::Bits256> key_id;
  td::optional<std::string> key_name;
  td::uint32 key_idx = 0;
  double timeout = 5.0;

  td::OptionParser p;
  p.set_description("find value in dht by the given key (key-id, key-name, ket-idx)");
  p.add_option('h', "help", "print help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('C', "global-config", "global config", [&](td::Slice arg) { global_config = arg.str(); });
  p.add_checked_option('s', "server-idx", "index of dht server from global config (default: all)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(server_idx, td::to_integer_safe<int>(arg));
    return td::Status::OK();
  });
  p.add_checked_option('p', "port", "set udp port", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(port, td::to_integer_safe<td::uint16>(arg));
    return td::Status::OK();
  });
  p.add_option('v', "verbosity", "set verbosity", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_checked_option('k', "key-id", "set key id (256-bit, base64)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(key_id, parse_bits256(arg));
    return td::Status::OK();
  });
  p.add_option('n', "key-name", "set key name", [&](td::Slice arg) { key_name = arg.str(); });
  p.add_checked_option('i', "key-idx", "set key idx (default: 0)", [&](td::Slice arg) {
    TRY_RESULT_ASSIGN(key_idx, td::to_integer_safe<td::uint32>(arg));
    return td::Status::OK();
  });
  p.add_option('t', "timeout", "set timeout (default: 5s)", [&](td::Slice arg) { timeout = td::to_double(arg); });

  td::actor::Scheduler scheduler({2});

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] {
    LOG_IF(FATAL, !global_config) << "global config is not set";
    LOG_IF(FATAL, !key_id) << "key-id is not set";
    LOG_IF(FATAL, !key_name) << "key-name is not set";
    x = td::actor::create_actor<Resolver>(
        "Resolver", global_config.value(), server_idx, port,
        ton::dht::DhtKey{ton::PublicKeyHash(key_id.value()), key_name.value(), key_idx}, timeout);
  });
  scheduler.run_in_context([&] { td::actor::send_closure(x, &Resolver::run); });

  scheduler.run();

  return 0;
}
