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
#include "download-proof.hpp"
#include "ton/ton-tl.hpp"
#include "adnl/utils.hpp"
#include "ton/ton-shard.h"
#include "td/utils/overloaded.h"
#include "ton/ton-io.hpp"
#include "validator/full-node.h"

namespace ton {

namespace validator {

namespace fullnode {

DownloadProof::DownloadProof(BlockIdExt block_id, bool allow_partial_proof, bool is_key_block,
                             adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id,
                             adnl::AdnlNodeIdShort download_from, td::uint32 priority, td::Timestamp timeout,
                             td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                             td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                             td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<adnl::AdnlExtClient> client,
                             td::Promise<td::BufferSlice> promise)
    : block_id_(block_id)
    , allow_partial_proof_(allow_partial_proof)
    , is_key_block_(is_key_block)
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

void DownloadProof::abort_query(td::Status reason) {
  if (promise_) {
    if (reason.code() == ErrorCode::notready || reason.code() == ErrorCode::timeout) {
      VLOG(FULL_NODE_DEBUG) << "failed to download proof " << block_id_ << "from " << download_from_ << ": " << reason;
    } else {
      VLOG(FULL_NODE_NOTICE) << "failed to download proof " << block_id_ << " from " << download_from_ << ": "
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
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::unique_ptr<DownloadToken>> R) {
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

void DownloadProof::got_download_token(std::unique_ptr<DownloadToken> token) {
  token_ = std::move(token);

  if (download_from_.is_zero() && client_.empty()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadProof::abort_query, R.move_as_error());
      } else {
        auto vec = R.move_as_ok();
        if (vec.size() == 0) {
          td::actor::send_closure(SelfId, &DownloadProof::abort_query,
                                  td::Status::Error(ErrorCode::notready, "no nodes"));
        } else {
          td::actor::send_closure(SelfId, &DownloadProof::got_node_to_download, vec[0]);
        }
      }
    });

    td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 1,
                            std::move(P));
  } else {
    got_node_to_download(download_from_);
  }
}

void DownloadProof::got_node_to_download(adnl::AdnlNodeIdShort node) {
  download_from_ = node;
  VLOG(FULL_NODE_DEBUG) << "downloading proof for " << block_id_;

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

  if (client_.empty()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                            "get_prepare", std::move(P), td::Timestamp::in(1.0), std::move(query));
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_prepare",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)),
                            td::Timestamp::in(1.0), std::move(P));
  }
}

void DownloadProof::got_block_proof_description(td::BufferSlice proof_description) {
  VLOG(FULL_NODE_DEBUG) << "downloaded proof description for " << block_id_;

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
            if (client_.empty()) {
              td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_,
                                      overlay_id_, "download block proof", std::move(P), td::Timestamp::in(3.0),
                                      std::move(query), FullNode::max_proof_size(), rldp_);
            } else {
              td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_prepare",
                                      create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)),
                                      td::Timestamp::in(3.0), std::move(P));
            }
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
            if (client_.empty()) {
              td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_,
                                      overlay_id_, "download block proof link", std::move(P), td::Timestamp::in(3.0),
                                      std::move(query), FullNode::max_proof_size(), rldp_);
            } else {
              td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "download block proof link",
                                      create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)),
                                      td::Timestamp::in(3.0), std::move(P));
            }
          },
          [&](ton_api::tonNode_preparedProofEmpty &obj) {
            abort_query(td::Status::Error(ErrorCode::notready, "proof not found"));
          }));
}

void DownloadProof::got_block_proof(td::BufferSlice proof) {
  VLOG(FULL_NODE_DEBUG) << "downloaded proof for " << block_id_;

  data_ = std::move(proof);
  finish_query();
}

void DownloadProof::got_block_partial_proof(td::BufferSlice proof) {
  VLOG(FULL_NODE_DEBUG) << "downloaded partial proof for " << block_id_;

  data_ = std::move(proof);
  finish_query();
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
