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

#include <vector>
#include <utility>

#include "ton/ton-types.h"

#include "td/actor/actor.h"

#include "adnl/adnl.h"
#include "rldp/rldp.h"
#include "dht/dht.h"
#include "overlay/overlays.h"
#include "validator/validator.h"
#include "adnl/adnl-ext-client.h"

namespace ton {

namespace validator {

namespace fullnode {

constexpr int VERBOSITY_NAME(FULL_NODE_WARNING) = verbosity_WARNING;
constexpr int VERBOSITY_NAME(FULL_NODE_NOTICE) = verbosity_INFO;
constexpr int VERBOSITY_NAME(FULL_NODE_INFO) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(FULL_NODE_DEBUG) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(FULL_NODE_EXTRA_DEBUG) = verbosity_DEBUG + 1;

class FullNode : public td::actor::Actor {
 public:
  virtual ~FullNode() = default;

  virtual void update_dht_node(td::actor::ActorId<dht::Dht> dht) = 0;

  virtual void add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) = 0;
  virtual void del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) = 0;

  virtual void sign_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                              td::uint32 expiry_at, td::uint32 max_size,
                                              td::Promise<td::BufferSlice> promise) = 0;
  virtual void import_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                                std::shared_ptr<ton::overlay::Certificate> cert,
                                                td::Promise<td::Unit> promise) = 0;

  virtual void update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) = 0;

  static constexpr td::uint32 max_block_size() {
    return 4 << 20;
  }
  static constexpr td::uint32 max_proof_size() {
    return 4 << 20;
  }
  static constexpr td::uint64 max_state_size() {
    return 4ull << 30;
  }

  static td::actor::ActorOwn<FullNode> create(ton::PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id,
                                              FileHash zero_state_file_hash,
                                              td::actor::ActorId<keyring::Keyring> keyring,
                                              td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
                                              td::actor::ActorId<dht::Dht> dht,
                                              td::actor::ActorId<overlay::Overlays> overlays,
                                              td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                              td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root);
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
