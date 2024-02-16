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
*/
#pragma once

#include "full-node.h"

namespace ton {

namespace validator {

namespace fullnode {

class FullNodePrivateOverlay : public td::actor::Actor {
 public:
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query);
  void process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query);
  template <class T>
  void process_broadcast(PublicKeyHash, T &) {
    VLOG(FULL_NODE_WARNING) << "dropping unknown broadcast";
  }
  void receive_broadcast(PublicKeyHash src, td::BufferSlice query);

  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data);
  void send_broadcast(BlockBroadcast broadcast);

  void start_up() override;
  void tear_down() override;

  FullNodePrivateOverlay(adnl::AdnlNodeIdShort local_id, std::vector<adnl::AdnlNodeIdShort> nodes,
                         FileHash zero_state_file_hash, FullNodeConfig config,
                         td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                         td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<rldp2::Rldp> rldp2,
                         td::actor::ActorId<overlay::Overlays> overlays,
                         td::actor::ActorId<ValidatorManagerInterface> validator_manager)
      : local_id_(local_id)
      , nodes_(std::move(nodes))
      , zero_state_file_hash_(zero_state_file_hash)
      , config_(config)
      , keyring_(keyring)
      , adnl_(adnl)
      , rldp_(rldp)
      , rldp2_(rldp2)
      , overlays_(overlays)
      , validator_manager_(validator_manager) {
  }

 private:
  adnl::AdnlNodeIdShort local_id_;
  std::vector<adnl::AdnlNodeIdShort> nodes_;
  FileHash zero_state_file_hash_;
  FullNodeConfig config_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<rldp2::Rldp> rldp2_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;

  bool inited_ = false;
  overlay::OverlayIdFull overlay_id_full_;
  overlay::OverlayIdShort overlay_id_;

  void try_init();
  void init();
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
