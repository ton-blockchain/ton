/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <set>

#include "td/utils/int_types.h"
#include "td/actor/actor.h"
#include "td/utils/List.h"

#include "adnl/adnl.h"

#include "adnl/utils.hpp"
#include "keys/encryptor.h"

#include "dht.h"
#include "dht-node.hpp"

#include "auto/tl/ton_api.hpp"

namespace ton {

namespace dht {

constexpr int VERBOSITY_NAME(DHT_WARNING) = verbosity_INFO;
constexpr int VERBOSITY_NAME(DHT_NOTICE) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(DHT_INFO) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(DHT_DEBUG) = verbosity_DEBUG + 1;
constexpr int VERBOSITY_NAME(DHT_EXTRA_DEBUG) = verbosity_DEBUG + 10;

class DhtGlobalConfig {
 public:
  auto get_k() const {
    return k_;
  }
  auto get_a() const {
    return a_;
  }
  auto get_network_id() const {
    return network_id_;
  }
  const auto &nodes() const {
    return static_nodes_;
  }
  DhtGlobalConfig(td::uint32 k, td::uint32 a, td::int32 network_id, DhtNodesList nodes)
      : k_(k), a_(a), network_id_(network_id), static_nodes_(std::move(nodes)) {
  }

 private:
  td::uint32 k_;
  td::uint32 a_;
  td::int32 network_id_;
  DhtNodesList static_nodes_;
};

class DhtMember : public Dht {
 public:
  static constexpr td::uint32 default_k() {
    return 10;
  }
  static constexpr td::uint32 default_a() {
    return 3;
  }
  static constexpr td::uint32 max_k() {
    return 10;
  }
  static constexpr td::uint32 max_a() {
    return 10;
  }

  struct PrintId {
    adnl::AdnlNodeIdShort id;
  };

  static td::actor::ActorOwn<DhtMember> create(adnl::AdnlNodeIdShort id, std::string db_root,
                                               td::actor::ActorId<keyring::Keyring> keyring,
                                               td::actor::ActorId<adnl::Adnl> adnl, td::int32 network_id,
                                               td::uint32 k = 10, td::uint32 a = 3, bool client_only = false);

  //virtual void update_addr_list(tl_object_ptr<ton_api::adnl_addressList> addr_list) = 0;
  //virtual void add_node(adnl::AdnlNodeIdShort id) = 0;
  virtual void add_full_node(DhtKeyId id, DhtNode node) = 0;

  virtual void receive_ping(DhtKeyId id, DhtNode result) = 0;

  virtual void get_value_in(DhtKeyId key, td::Promise<DhtValue> result) = 0;

  virtual td::Status store_in(DhtValue value) = 0;

  virtual void get_self_node(td::Promise<DhtNode> promise) = 0;

  virtual PrintId print_id() const = 0;

  static DhtKey get_reverse_connection_key(adnl::AdnlNodeIdShort node) {
    return DhtKey{node.pubkey_hash(), "address", 0};
  }
};

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const DhtMember::PrintId &id) {
  sb << "[dhtnode " << id.id << "]";
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const DhtMember &dht) {
  sb << dht.print_id();
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const DhtMember *dht) {
  sb << dht->print_id();
  return sb;
}

}  // namespace dht

}  // namespace ton
