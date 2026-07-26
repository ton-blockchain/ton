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
#include "td/utils/overloaded.h"
#include "ton/ton-io.hpp"
#include "ton/ton-shard.h"
#include "ton/ton-tl.hpp"
#include "validator/full-node.h"

#include "download-proof.hpp"

namespace ton {

namespace validator {

namespace fullnode {

DownloadProof::DownloadProof(BlockIdExt block_id, bool allow_partial_proof, bool is_key_block, QuerySender query_sender,
                             td::uint32 priority, td::Timestamp timeout,
                             td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                             td::Promise<td::BufferSlice> promise)
    : block_id_(block_id)
    , allow_partial_proof_(allow_partial_proof)
    , is_key_block_(is_key_block)
    , query_sender_(std::move(query_sender))
    , priority_(priority)
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , promise_(std::move(promise)) {
}

void DownloadProof::abort_query(td::Status reason) {
  if (promise_) {
    if (reason.code() == ErrorCode::notready || reason.code() == ErrorCode::timeout) {
      VLOG(full_node, DEBUG) << "failed to download proof " << block_id_ << " from " << query_sender_->to_str() << ": "
                             << reason;
    } else {
      VLOG(full_node, INFO) << "failed to download proof " << block_id_ << " from " << query_sender_->to_str() << ": "
                            << reason;
    }
    promise_.set_error(std::move(reason));
  }
  stop();
}

void DownloadProof::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadProof::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(data_));
  }
  stop();
}

void DownloadProof::start_up() {
  alarm_timestamp() = timeout_;

  if (!block_id_.is_masterchain()) {
    checked_db();
    return;
  }

  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this), l = allow_partial_proof_](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &DownloadProof::checked_db);
        } else {
          if (l) {
            td::actor::send_closure(SelfId, &DownloadProof::got_block_partial_proof, R.move_as_ok());
          } else {
            td::actor::send_closure(SelfId, &DownloadProof::got_block_proof, R.move_as_ok());
          }
        }
      });
  if (allow_partial_proof_) {
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_key_block_proof_link, block_id_,
                            std::move(P));
  } else {
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_key_block_proof, block_id_,
                            std::move(P));
  }
}

void DownloadProof::checked_db() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::unique_ptr<ActionToken>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadProof::abort_query,
                              R.move_as_error_prefix("failed to get download token: "));
    } else {
      td::actor::send_closure(SelfId, &DownloadProof::got_download_token, R.move_as_ok());
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_download_token, 1, priority_, timeout_,
                          std::move(P));
}

void DownloadProof::got_download_token(std::unique_ptr<ActionToken> token) {
  token_ = std::move(token);
  VLOG(full_node, DEBUG) << "downloading proof for " << block_id_ << " from " << query_sender_->to_str();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadProof::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadProof::got_block_proof_description, R.move_as_ok());
    }
  });

  td::BufferSlice query;
  if (!is_key_block_) {
    query = create_serialize_tl_object<ton_api::tonNode_prepareBlockProof>(create_tl_block_id(block_id_),
                                                                           allow_partial_proof_);
  } else {
    query = create_serialize_tl_object<ton_api::tonNode_prepareKeyBlockProof>(create_tl_block_id(block_id_),
                                                                              allow_partial_proof_);
  }
  query_sender_->send_query(std::move(query), td::Timestamp::in(1.0), 1024, std::move(P));
}

void DownloadProof::got_block_proof_description(td::BufferSlice proof_description) {
  VLOG(full_node, DEBUG) << "downloaded proof description for " << block_id_;

  auto F = fetch_tl_object<ton_api::tonNode_PreparedProof>(std::move(proof_description), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  auto self = this;
  ton_api::downcast_call(
      *F.move_as_ok().get(),
      td::overloaded(
          [&](ton_api::tonNode_preparedProof &obj) {
            auto P = td::PromiseCreator::lambda([SelfId = actor_id(self)](td::Result<td::BufferSlice> R) {
              if (R.is_error()) {
                td::actor::send_closure(SelfId, &DownloadProof::abort_query, R.move_as_error());
              } else {
                td::actor::send_closure(SelfId, &DownloadProof::got_block_proof, R.move_as_ok());
              }
            });

            td::BufferSlice query;
            if (!is_key_block_) {
              query = create_serialize_tl_object<ton_api::tonNode_downloadBlockProof>(create_tl_block_id(block_id_));
            } else {
              query = create_serialize_tl_object<ton_api::tonNode_downloadKeyBlockProof>(create_tl_block_id(block_id_));
            }
            query_sender_->send_query(std::move(query), td::Timestamp::in(3.0), FullNode::max_proof_size(),
                                      std::move(P));
          },
          [&](ton_api::tonNode_preparedProofLink &obj) {
            if (!allow_partial_proof_) {
              abort_query(td::Status::Error(ErrorCode::protoviolation, "received partial proof, though did not allow"));
              return;
            }
            auto P = td::PromiseCreator::lambda([SelfId = actor_id(self)](td::Result<td::BufferSlice> R) {
              if (R.is_error()) {
                td::actor::send_closure(SelfId, &DownloadProof::abort_query, R.move_as_error());
              } else {
                td::actor::send_closure(SelfId, &DownloadProof::got_block_partial_proof, R.move_as_ok());
              }
            });

            td::BufferSlice query;
            if (!is_key_block_) {
              query =
                  create_serialize_tl_object<ton_api::tonNode_downloadBlockProofLink>(create_tl_block_id(block_id_));
            } else {
              query =
                  create_serialize_tl_object<ton_api::tonNode_downloadKeyBlockProofLink>(create_tl_block_id(block_id_));
            }
            query_sender_->send_query(std::move(query), td::Timestamp::in(3.0), FullNode::max_proof_size(),
                                      std::move(P));
          },
          [&](ton_api::tonNode_preparedProofEmpty &obj) {
            abort_query(td::Status::Error(ErrorCode::notready, "proof not found"));
          }));
}

void DownloadProof::got_block_proof(td::BufferSlice proof) {
  VLOG(full_node, DEBUG) << "downloaded proof for " << block_id_ << " from " << query_sender_->to_str();
  data_ = std::move(proof);
  finish_query();
}

void DownloadProof::got_block_partial_proof(td::BufferSlice proof) {
  VLOG(full_node, DEBUG) << "downloaded partial proof for " << block_id_ << " from " << query_sender_->to_str();
  data_ = std::move(proof);
  finish_query();
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
