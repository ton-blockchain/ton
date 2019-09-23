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
#include "download-block-new.hpp"
#include "ton/ton-tl.hpp"
#include "adnl/utils.hpp"
#include "ton/ton-shard.h"
#include "td/utils/overloaded.h"
#include "ton/ton-io.hpp"
#include "validator/full-node.h"

namespace ton {

namespace validator {

namespace fullnode {

DownloadBlockNew::DownloadBlockNew(BlockIdExt block_id, adnl::AdnlNodeIdShort local_id,
                                   overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort download_from,
                                   td::uint32 priority, td::Timestamp timeout,
                                   td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                   td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                                   td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<adnl::AdnlExtClient> client,
                                   td::Promise<ReceivedBlock> promise)
    : block_id_(block_id)
    , local_id_(local_id)
    , overlay_id_(overlay_id)
    , download_from_(download_from)
    , priority_(priority)
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , rldp_(rldp)
    , overlays_(overlays)
    , adnl_(adnl)
    , client_(client)
    , promise_(std::move(promise))
    , block_{block_id_, td::BufferSlice()}
    , allow_partial_proof_{!block_id_.is_masterchain()} {
}

DownloadBlockNew::DownloadBlockNew(adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id,
                                   BlockIdExt prev_id, adnl::AdnlNodeIdShort download_from, td::uint32 priority,
                                   td::Timestamp timeout,
                                   td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                   td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                                   td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<adnl::AdnlExtClient> client,
                                   td::Promise<ReceivedBlock> promise)
    : local_id_(local_id)
    , overlay_id_(overlay_id)
    , prev_id_(prev_id)
    , download_from_(download_from)
    , priority_(priority)
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , rldp_(rldp)
    , overlays_(overlays)
    , adnl_(adnl)
    , client_(client)
    , promise_(std::move(promise))
    , block_{BlockIdExt{}, td::BufferSlice()} {
}

void DownloadBlockNew::abort_query(td::Status reason) {
  if (promise_) {
    if (reason.code() == ErrorCode::notready || reason.code() == ErrorCode::timeout) {
      VLOG(FULL_NODE_DEBUG) << "failed to download block " << block_id_ << "from " << download_from_ << ": " << reason;
    } else {
      VLOG(FULL_NODE_NOTICE) << "failed to download block " << block_id_ << " from " << download_from_ << ": "
                             << reason;
    }
    promise_.set_error(std::move(reason));
  }
  stop();
}

void DownloadBlockNew::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadBlockNew::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(block_));
  }
  stop();
}

void DownloadBlockNew::start_up() {
  alarm_timestamp() = timeout_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadBlockNew::got_block_handle, R.move_as_ok());
  });

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle,
                          block_id_.is_valid() ? block_id_ : prev_id_, true, std::move(P));
}

void DownloadBlockNew::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);

  if (!block_id_.is_valid()) {
    CHECK(prev_id_.is_valid());
    if (handle_->inited_next_left()) {
      block_id_ = handle_->one_next(true);
      block_.id = block_id_;
      handle_ = nullptr;
      start_up();
      return;
    }
  }

  if (block_id_.is_valid() &&
      (handle_->inited_proof() || (handle_->inited_proof_link() && allow_partial_proof_) || skip_proof_) &&
      handle_->received()) {
    CHECK(block_.id == block_id_);
    CHECK(handle_->id() == block_id_);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadBlockNew::abort_query,
                                R.move_as_error_prefix("failed to get from db: "));
      } else {
        td::actor::send_closure(SelfId, &DownloadBlockNew::got_data_from_db, R.move_as_ok()->data());
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_data_from_db, handle_,
                            std::move(P));
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::unique_ptr<DownloadToken>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadBlockNew::abort_query,
                              R.move_as_error_prefix("failed to get download token: "));
    } else {
      td::actor::send_closure(SelfId, &DownloadBlockNew::got_download_token, R.move_as_ok());
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_download_token, 1, priority_, timeout_,
                          std::move(P));
}

void DownloadBlockNew::got_download_token(std::unique_ptr<DownloadToken> token) {
  token_ = std::move(token);

  if (download_from_.is_zero() && client_.empty()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadBlockNew::abort_query, R.move_as_error());
      } else {
        auto vec = R.move_as_ok();
        if (vec.size() == 0) {
          td::actor::send_closure(SelfId, &DownloadBlockNew::abort_query,
                                  td::Status::Error(ErrorCode::notready, "no nodes"));
        } else {
          td::actor::send_closure(SelfId, &DownloadBlockNew::got_node_to_download, vec[0]);
        }
      }
    });

    td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 1,
                            std::move(P));
  } else {
    got_node_to_download(download_from_);
  }
}

void DownloadBlockNew::got_node_to_download(adnl::AdnlNodeIdShort node) {
  download_from_ = node;

  VLOG(FULL_NODE_DEBUG) << "downloading proof for " << block_id_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadBlockNew::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadBlockNew::got_data, R.move_as_ok());
    }
  });

  td::BufferSlice q;
  if (block_id_.is_valid()) {
    q = create_serialize_tl_object<ton_api::tonNode_downloadBlockFull>(create_tl_block_id(block_id_));
  } else {
    q = create_serialize_tl_object<ton_api::tonNode_downloadNextBlockFull>(create_tl_block_id(prev_id_));
  }
  if (client_.empty()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_, overlay_id_,
                            "get_proof", std::move(P), td::Timestamp::in(3.0), std::move(q),
                            FullNode::max_proof_size() + FullNode::max_block_size() + 128, rldp_);
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_prepare",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                            td::Timestamp::in(1.0), std::move(P));
  }
}

void DownloadBlockNew::got_data(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::tonNode_DataFull>(std::move(data), true);

  if (F.is_error()) {
    abort_query(F.move_as_error_prefix("received invalid answer: "));
    return;
  }

  auto f = F.move_as_ok();

  ton_api::downcast_call(
      *f.get(),
      td::overloaded(
          [&](ton_api::tonNode_dataFullEmpty &x) {
            abort_query(td::Status::Error(ErrorCode::notready, "node doesn't have this block"));
          },
          [&](ton_api::tonNode_dataFull &x) {
            if (!allow_partial_proof_ && x.is_link_) {
              abort_query(td::Status::Error(ErrorCode::notready, "node doesn't have proof for this block"));
              return;
            }
            auto id = create_block_id(x.id_);
            if (block_id_.is_valid() && id != block_id_) {
              abort_query(td::Status::Error(ErrorCode::notready, "received data for wrong block"));
              return;
            }
            block_.id = id;
            block_.data = std::move(x.block_);
            if (td::sha256_bits256(block_.data.as_slice()) != id.file_hash) {
              abort_query(td::Status::Error(ErrorCode::notready, "received data with bad hash"));
              return;
            }

            auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
              if (R.is_error()) {
                td::actor::send_closure(SelfId, &DownloadBlockNew::abort_query,
                                        R.move_as_error_prefix("received bad proof: "));
              } else {
                td::actor::send_closure(SelfId, &DownloadBlockNew::checked_block_proof);
              }
            });
            if (block_id_.is_valid()) {
              if (x.is_link_) {
                td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_proof_link,
                                        block_id_, std::move(x.proof_), std::move(P));
              } else {
                td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_proof, block_id_,
                                        std::move(x.proof_), std::move(P));
              }
            } else {
              CHECK(!x.is_link_);
              td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_is_next_proof,
                                      prev_id_, id, std::move(x.proof_), std::move(P));
            }
          }));
}

void DownloadBlockNew::got_data_from_db(td::BufferSlice data) {
  block_.data = std::move(data);
  finish_query();
}

void DownloadBlockNew::checked_block_proof() {
  finish_query();
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
