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

#include "adnl/adnl-ext-client.h"
#include "overlay/overlays.h"
#include "ton/ton-types.h"
#include "validator/validator.h"

namespace ton {

namespace validator {

namespace fullnode {

class DownloadNextBlocks : public td::actor::Actor {
 public:
  DownloadNextBlocks(adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id, BlockHandle handle,
                     adnl::AdnlNodeIdShort download_from, td::uint32 priority, bool allow_many,
                     td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                     td::actor::ActorId<adnl::AdnlSenderInterface> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                     td::actor::ActorId<adnl::AdnlExtClient> client, td::Promise<BlockHandle> promise);

  void start_up() override;
  td::actor::Task<> run();
  td::actor::Task<> process_block(tl_object_ptr<ton_api::tonNode_DataFull> obj);

 private:
  adnl::AdnlNodeIdShort local_id_;
  overlay::OverlayIdShort overlay_id_;
  BlockHandle handle_;
  BlockIdExt start_prev_id_;

  adnl::AdnlNodeIdShort download_from_ = adnl::AdnlNodeIdShort::zero();

  td::uint32 priority_;

  bool allow_many_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<adnl::AdnlSenderInterface> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;
  td::Promise<BlockHandle> promise_;

  bool success_ = false;
  bool success_local_ = false;

  std::unique_ptr<ActionToken> token_;

  static constexpr td::uint32 MAX_BLOCKS = 10;
  static constexpr size_t MAX_SIZE_MANY = (1 << 20) + 128;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
