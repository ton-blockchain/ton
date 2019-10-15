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

    Copyright 2017-2019 Telegram Systems LLP
*/
#pragma once

#include "overlay/overlays.h"
#include "ton/ton-types.h"
#include "validator/validator.h"
#include "rldp/rldp.h"
#include "ton/ton-io.hpp"
#include "adnl/adnl-ext-client.h"

namespace ton {

namespace validator {

namespace fullnode {

class DownloadNextBlock : public td::actor::Actor {
 public:
  DownloadNextBlock(adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id, BlockHandle prev,
                    adnl::AdnlNodeIdShort download_from, td::uint32 priority, td::Timestamp timeout,
                    td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                    td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<adnl::AdnlExtClient> client,
                    td::Promise<ReceivedBlock> promise);

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;

  void start_up() override;
  void got_node(adnl::AdnlNodeIdShort id);
  void got_next_node(td::BufferSlice data);
  void got_next_node_handle(BlockHandle handle);

 private:
  adnl::AdnlNodeIdShort local_id_;
  overlay::OverlayIdShort overlay_id_;

  BlockHandle prev_;

  BlockIdExt next_block_id_;

  adnl::AdnlNodeIdShort download_from_ = adnl::AdnlNodeIdShort::zero();

  td::uint32 priority_;

  td::Timestamp timeout_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;
  td::Promise<ReceivedBlock> promise_;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
