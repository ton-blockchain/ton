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
#include "get-next-key-blocks.hpp"
#include "download-proof.hpp"
#include "ton/ton-tl.hpp"
#include "adnl/utils.hpp"
#include "ton/ton-shard.h"
#include "td/utils/overloaded.h"
#include "ton/ton-io.hpp"
#include "full-node.h"

namespace ton {

namespace validator {

namespace fullnode {

GetNextKeyBlocks::GetNextKeyBlocks(BlockIdExt block_id, td::uint32 limit, adnl::AdnlNodeIdShort local_id,
                                   overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort download_from,
                                   td::uint32 priority, td::Timestamp timeout,
                                   td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                   td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                                   td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<adnl::AdnlExtClient> client,
                                   td::Promise<std::vector<BlockIdExt>> promise)
    : block_id_(block_id)
    , limit_(limit)
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
    , promise_(std::move(promise)) {
}

void GetNextKeyBlocks::abort_query(td::Status reason) {
  if (promise_) {
    if (reason.code() == ErrorCode::notready || reason.code() == ErrorCode::timeout) {
      VLOG(FULL_NODE_DEBUG) << "failed to download proof " << block_id_ << "from " << download_from_ << ": " << reason;
    } else {
      VLOG(FULL_NODE_NOTICE) << "failed to download proof " << block_id_ << " from " << download_from_ << ": "
                             << reason;
    }
    if (res_.size() > 0) {
      promise_.set_value(std::move(res_));
    } else {
      promise_.set_error(std::move(reason));
    }
  }
  stop();
}

void GetNextKeyBlocks::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void GetNextKeyBlocks::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(res_));
  }
  stop();
}

void GetNextKeyBlocks::start_up() {
  alarm_timestamp() = timeout_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::unique_ptr<DownloadToken>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &GetNextKeyBlocks::abort_query,
                              R.move_as_error_prefix("failed to get download token: "));
    } else {
      td::actor::send_closure(SelfId, &GetNextKeyBlocks::got_download_token, R.move_as_ok());
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_download_token, 1, priority_, timeout_,
                          std::move(P));
}

void GetNextKeyBlocks::got_download_token(std::unique_ptr<DownloadToken> token) {
  token_ = std::move(token);

  if (download_from_.is_zero() && client_.empty()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &GetNextKeyBlocks::abort_query, R.move_as_error());
      } else {
        auto vec = R.move_as_ok();
        if (vec.size() == 0) {
          td::actor::send_closure(SelfId, &GetNextKeyBlocks::abort_query,
                                  td::Status::Error(ErrorCode::notready, "no nodes"));
        } else {
          td::actor::send_closure(SelfId, &GetNextKeyBlocks::got_node_to_download, vec[0]);
        }
      }
    });

    td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 1,
                            std::move(P));
  } else {
    got_node_to_download(download_from_);
  }
}

void GetNextKeyBlocks::got_node_to_download(adnl::AdnlNodeIdShort node) {
  download_from_ = node;
  VLOG(FULL_NODE_DEBUG) << "downloading proof for " << block_id_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &GetNextKeyBlocks::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &GetNextKeyBlocks::got_result, R.move_as_ok());
    }
  });
  auto query = create_serialize_tl_object<ton_api::tonNode_getNextKeyBlockIds>(create_tl_block_id(block_id_), limit_);
  if (client_.empty()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                            "get_prepare", std::move(P), td::Timestamp::in(1.0), std::move(query));
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_prepare",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)),
                            td::Timestamp::in(1.0), std::move(P));
  }
}

void GetNextKeyBlocks::got_result(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::tonNode_keyBlocks>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error_prefix("received bad answer: "));
    return;
  }
  auto f = F.move_as_ok();
  if (f->error_) {
    abort_query(td::Status::Error(ErrorCode::notready, "received answer with error"));
    return;
  }

  VLOG(FULL_NODE_DEBUG) << "received " << f->blocks_.size() << " key blocks";
  for (auto &x : f->blocks_) {
    pending_.push_back(create_block_id(x));
  }

  download_next_proof();
}

void GetNextKeyBlocks::download_next_proof() {
  if (!pending_.size()) {
    finish_query();
    return;
  }
  auto block_id = pending_[0];

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &GetNextKeyBlocks::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &GetNextKeyBlocks::got_next_proof, R.move_as_ok());
    }
  });

  td::actor::create_actor<DownloadProof>("downloadproofreq", block_id, false, true, local_id_, overlay_id_,
                                         download_from_, priority_, timeout_, validator_manager_, rldp_, overlays_,
                                         adnl_, client_, std::move(P))
      .release();
}

void GetNextKeyBlocks::got_next_proof(td::BufferSlice proof) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &GetNextKeyBlocks::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &GetNextKeyBlocks::checked_next_proof);
    }
  });

  auto block_id = pending_[0];

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_proof_rel, block_id,
                          (res_.size() > 0) ? *res_.rbegin() : block_id_, std::move(proof), std::move(P));
}

void GetNextKeyBlocks::checked_next_proof() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &GetNextKeyBlocks::got_next_block_handle, R.move_as_ok());
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle, pending_[0], false,
                          std::move(P));
}

void GetNextKeyBlocks::got_next_block_handle(BlockHandle handle) {
  CHECK(handle->inited_is_key_block());
  if (!handle->is_key_block()) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "got not key block"));
    return;
  }
  auto block_id = pending_[0];
  res_.push_back(block_id);
  pending_.erase(pending_.begin());
  download_next_proof();
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
