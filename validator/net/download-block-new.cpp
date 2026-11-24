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
#include "adnl/utils.hpp"
#include "ton/ton-io.hpp"
#include "ton/ton-shard.h"
#include "ton/ton-tl.hpp"
#include "validator/full-node.h"

#include "download-block-new.hpp"
#include "full-node-serializer.hpp"

namespace ton {

namespace validator {

namespace fullnode {

DownloadBlockNew::DownloadBlockNew(BlockIdExt block_id, QuerySender query_sender, td::uint32 priority,
                                   td::Timestamp timeout,
                                   td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                   td::Promise<ReceivedBlock> promise)
    : block_id_(block_id)
    , query_sender_(std::move(query_sender))
    , priority_(priority)
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , promise_(std::move(promise))
    , block_{block_id_, td::BufferSlice()}
    , allow_partial_proof_{!block_id_.is_masterchain()} {
}

void DownloadBlockNew::abort_query(td::Status reason) {
  if (promise_) {
    if (reason.code() == ErrorCode::notready || reason.code() == ErrorCode::timeout) {
      VLOG(full_node, DEBUG) << "failed to download block " << block_id_ << " from " << query_sender_->to_str() << ": "
                             << reason;
    } else {
      VLOG(full_node, INFO) << "failed to download block " << block_id_ << " from " << query_sender_->to_str() << ": "
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
    VLOG(full_node, DEBUG) << "downloaded block " << block_id_ << " from " << query_sender_->to_str();
    promise_.set_value(std::move(block_));
  }
  stop();
}

void DownloadBlockNew::start_up() {
  if (!block_id_.is_valid()) {
    abort_query(td::Status::Error("invalid block id"));
    return;
  }
  alarm_timestamp() = timeout_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &DownloadBlockNew::got_block_handle, R.move_as_ok());
  });

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle, block_id_, true,
                          std::move(P));
}

void DownloadBlockNew::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);

  if ((handle_->inited_proof() || (handle_->inited_proof_link() && allow_partial_proof_) || skip_proof_) &&
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

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::unique_ptr<ActionToken>> R) {
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

void DownloadBlockNew::got_download_token(std::unique_ptr<ActionToken> token) {
  token_ = std::move(token);
  VLOG(full_node, DEBUG) << "downloading proof for " << block_id_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadBlockNew::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadBlockNew::got_data, R.move_as_ok());
    }
  });

  query_sender_->send_query(
      create_serialize_tl_object<ton_api::tonNode_downloadBlockFull>(create_tl_block_id(block_id_)), timeout_,
      FullNode::max_proof_size() + FullNode::max_block_size() + 128, std::move(P));
}

void DownloadBlockNew::got_data(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::tonNode_DataFull>(std::move(data), true);

  if (F.is_error()) {
    abort_query(F.move_as_error_prefix("received invalid answer: "));
    return;
  }

  auto f = F.move_as_ok();
  if (f->get_id() == ton_api::tonNode_dataFullEmpty::ID) {
    abort_query(td::Status::Error(ErrorCode::notready, "node doesn't have this block"));
    return;
  }

  BlockIdExt id;
  td::BufferSlice proof, block_data;
  bool is_link;
  td::Status S =
      deserialize_block_full(*f, id, proof, block_data, is_link, overlay::Overlays::max_fec_broadcast_size());
  if (S.is_error()) {
    abort_query(S.move_as_error_prefix("cannot deserialize block: "));
    return;
  }

  if (!allow_partial_proof_ && is_link) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "node doesn't have proof for this block"));
    return;
  }
  if (id != block_id_) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "received data for wrong block"));
    return;
  }
  block_.id = id;
  block_.data = std::move(block_data);
  if (td::sha256_bits256(block_.data.as_slice()) != id.file_hash) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, "received data with bad hash"));
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadBlockNew::abort_query, R.move_as_error_prefix("received bad proof: "));
    } else {
      td::actor::send_closure(SelfId, &DownloadBlockNew::checked_block_proof);
    }
  });
  if (is_link) {
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_proof_link, block_id_,
                            std::move(proof), std::move(P));
  } else {
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_proof, block_id_,
                            std::move(proof), std::move(P));
  }
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
