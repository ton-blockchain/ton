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

#include "overlay/overlays.h"
#include "ton/ton-types.h"
#include "validator/validator.h"
#include "rldp/rldp.h"
#include "adnl/adnl-ext-client.h"

namespace ton {

namespace validator {

namespace fullnode {

class DownloadState : public td::actor::Actor {
 public:
  DownloadState(BlockIdExt block_id, BlockIdExt masterchain_block_id, adnl::AdnlNodeIdShort local_id,
                overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort download_from, td::uint32 priority,
                td::Timestamp timeout, td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<adnl::AdnlExtClient> client,
                td::Promise<td::BufferSlice> promise);

  void abort_query(td::Status reason);
  void alarm() override;
  void finish_query();

  void start_up() override;
  void got_block_handle(BlockHandle handle);
  void got_node_to_download(adnl::AdnlNodeIdShort node);
  void got_block_state_description(td::BufferSlice data_description);
  void got_block_state_part(td::BufferSlice data, td::uint32 requested_size);
  void got_block_state(td::BufferSlice data);

 private:
  BlockIdExt block_id_;
  BlockIdExt masterchain_block_id_;
  adnl::AdnlNodeIdShort local_id_;
  overlay::OverlayIdShort overlay_id_;

  adnl::AdnlNodeIdShort download_from_ = adnl::AdnlNodeIdShort::zero();

  td::uint32 priority_;

  td::Timestamp timeout_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;
  td::Promise<td::BufferSlice> promise_;

  BlockHandle handle_;
  td::BufferSlice state_;
  std::vector<td::BufferSlice> parts_;
  td::uint64 sum_ = 0;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
