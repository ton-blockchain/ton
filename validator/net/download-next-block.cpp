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
#include "download-next-block.hpp"
#include "ton/ton-tl.hpp"
#include "adnl/utils.hpp"
#include "download-block.hpp"
#include "validator/full-node.h"

namespace ton {

namespace validator {

namespace fullnode {

DownloadNextBlock::DownloadNextBlock(adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id,
                                     BlockHandle prev, adnl::AdnlNodeIdShort download_from, td::uint32 priority,
                                     td::Timestamp timeout,
                                     td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                     td::actor::ActorId<rldp::Rldp> rldp,
                                     td::actor::ActorId<overlay::Overlays> overlays,
                                     td::actor::ActorId<adnl::Adnl> adnl,
                                     td::actor::ActorId<adnl::AdnlExtClient> client, td::Promise<ReceivedBlock> promise)
    : local_id_(local_id)
    , overlay_id_(overlay_id)
    , prev_(prev)
    , download_from_(download_from)
    , priority_(priority)
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , rldp_(rldp)
    , overlays_(overlays)
    , adnl_(adnl)
    , client_(client)
    , promise_(std::move(promise)) {
}

void DownloadNextBlock::abort_query(td::Status reason) {
  if (promise_) {
    if (reason.code() == ErrorCode::notready || reason.code() == ErrorCode::timeout) {
      VLOG(FULL_NODE_DEBUG) << "failed to download next block after " << prev_->id() << " from " << download_from_
                            << ": " << reason;
    } else {
      VLOG(FULL_NODE_INFO) << "failed to download next block after " << prev_->id() << " from " << download_from_
                           << ": " << reason;
    }
    promise_.set_error(std::move(reason));
  }
  stop();
}

void DownloadNextBlock::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadNextBlock::start_up() {
  alarm_timestamp() = timeout_;

  if (prev_->inited_next_left()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &DownloadNextBlock::got_next_node_handle, R.move_as_ok());
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle, prev_->one_next(true),
                            false, std::move(P));
    return;
  }
  if (!client_.empty()) {
    got_node(download_from_);
    return;
  }

  if (download_from_.is_zero()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadNextBlock::abort_query, R.move_as_error());
      } else {
        auto vec = R.move_as_ok();
        if (vec.size() == 0) {
          td::actor::send_closure(SelfId, &DownloadNextBlock::abort_query,
                                  td::Status::Error(ErrorCode::notready, "no neighbours found"));
        } else {
          td::actor::send_closure(SelfId, &DownloadNextBlock::got_node, vec[0]);
        }
      }
    });

    td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 1,
                            std::move(P));
  } else {
    got_node(download_from_);
  }
}

void DownloadNextBlock::got_node(adnl::AdnlNodeIdShort id) {
  download_from_ = id;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadNextBlock::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadNextBlock::got_next_node, R.move_as_ok());
    }
  });

  auto query = create_serialize_tl_object<ton_api::tonNode_getNextBlockDescription>(create_tl_block_id(prev_->id()));
  if (client_.empty()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                            "get_prepare", std::move(P), td::Timestamp::in(1.0), std::move(query));
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_prepare",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)),
                            td::Timestamp::in(1.0), std::move(P));
  }
}

void DownloadNextBlock::got_next_node(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::tonNode_BlockDescription>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  if (f->get_id() == ton_api::tonNode_blockDescriptionEmpty::ID) {
    abort_query(td::Status::Error(ErrorCode::notready, "not found"));
    return;
  }
  auto g = move_tl_object_as<ton_api::tonNode_blockDescription>(std::move(f));

  next_block_id_ = create_block_id(g->id_);
  finish_query();
}

void DownloadNextBlock::got_next_node_handle(BlockHandle handle) {
  //CHECK(handle->inited_proof());
  //CHECK(handle->inited_masterchain());
  next_block_id_ = handle->id();
  finish_query();
}

void DownloadNextBlock::finish_query() {
  if (promise_) {
    td::actor::create_actor<DownloadBlock>("downloadnext", next_block_id_, local_id_, overlay_id_, prev_,
                                           download_from_, priority_, timeout_, validator_manager_, rldp_, overlays_,
                                           adnl_, client_, std::move(promise_))
        .release();
  }
  stop();
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
