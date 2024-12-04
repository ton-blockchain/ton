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
#include "download-block.hpp"
#include "ton/ton-tl.hpp"
#include "adnl/utils.hpp"
#include "ton/ton-shard.h"
#include "td/utils/overloaded.h"
#include "ton/ton-io.hpp"
#include "validator/full-node.h"

namespace ton {

namespace validator {

namespace fullnode {

DownloadBlock::DownloadBlock(BlockIdExt block_id, adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id,
                             adnl::AdnlNodeIdShort download_from, td::uint32 priority, td::Timestamp timeout,
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
    , block_{block_id, td::BufferSlice()}
    , allow_partial_proof_{!block_id_.is_masterchain()} {
}

DownloadBlock::DownloadBlock(BlockIdExt block_id, adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id,
                             BlockHandle prev, adnl::AdnlNodeIdShort download_from, td::uint32 priority,
                             td::Timestamp timeout, td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                             td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                             td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<adnl::AdnlExtClient> client,
                             td::Promise<ReceivedBlock> promise)
    : block_id_(block_id)
    , local_id_(local_id)
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
    , promise_(std::move(promise))
    , block_{block_id, td::BufferSlice()} {
}

void DownloadBlock::abort_query(td::Status reason) {
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

void DownloadBlock::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadBlock::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(block_));
  }
  stop();
}

void DownloadBlock::start_up() {
  alarm_timestamp() = timeout_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      auto S = R.move_as_error();
      if (S.code() == ErrorCode::notready) {
        td::actor::send_closure(SelfId, &DownloadBlock::got_block_handle, nullptr);
      } else {
        td::actor::send_closure(SelfId, &DownloadBlock::abort_query, std::move(S));
      }
    } else {
      td::actor::send_closure(SelfId, &DownloadBlock::got_block_handle, R.move_as_ok());
    }
  });

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle, block_id_, false,
                          std::move(P));
}

void DownloadBlock::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);

  if (handle_ && (handle_->inited_proof() || (handle_->inited_proof_link() && allow_partial_proof_) || skip_proof_) &&
      handle_->received()) {
    short_ = true;
    got_download_token(nullptr);
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::unique_ptr<ActionToken>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadBlock::abort_query,
                              R.move_as_error_prefix("failed to get download token: "));
    } else {
      td::actor::send_closure(SelfId, &DownloadBlock::got_download_token, R.move_as_ok());
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_download_token, 1, priority_, timeout_,
                          std::move(P));
}

void DownloadBlock::got_download_token(std::unique_ptr<ActionToken> token) {
  token_ = std::move(token);

  if (download_from_.is_zero() && !short_ && client_.empty()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadBlock::abort_query, R.move_as_error());
      } else {
        auto vec = R.move_as_ok();
        if (vec.size() == 0) {
          td::actor::send_closure(SelfId, &DownloadBlock::abort_query,
                                  td::Status::Error(ErrorCode::notready, "no nodes"));
        } else {
          td::actor::send_closure(SelfId, &DownloadBlock::got_node_to_download, vec[0]);
        }
      }
    });

    td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 1,
                            std::move(P));
  } else {
    got_node_to_download(download_from_);
  }
}

void DownloadBlock::got_node_to_download(adnl::AdnlNodeIdShort node) {
  download_from_ = node;
  if (skip_proof_ || (handle_ && (handle_->inited_proof() || (handle_->inited_proof_link() && allow_partial_proof_)))) {
    checked_block_proof();
    return;
  }

  VLOG(FULL_NODE_DEBUG) << "downloading proof for " << block_id_;

  CHECK(!short_);
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadBlock::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadBlock::got_block_proof_description, R.move_as_ok());
    }
  });

  auto q = create_serialize_tl_object<ton_api::tonNode_prepareBlockProof>(create_tl_block_id(block_id_),
                                                                          allow_partial_proof_);
  if (client_.empty()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                            "get_prepare", std::move(P), td::Timestamp::in(1.0), std::move(q));
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_prepare",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                            td::Timestamp::in(1.0), std::move(P));
  }
}

void DownloadBlock::got_block_proof_description(td::BufferSlice proof_description) {
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
                td::actor::send_closure(SelfId, &DownloadBlock::abort_query, R.move_as_error());
              } else {
                td::actor::send_closure(SelfId, &DownloadBlock::got_block_proof, R.move_as_ok());
              }
            });

            auto q = create_serialize_tl_object<ton_api::tonNode_downloadBlockProof>(create_tl_block_id(block_id_));
            if (client_.empty()) {
              td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_,
                                      overlay_id_, "get_proof", std::move(P), td::Timestamp::in(3.0), std::move(q),
                                      FullNode::max_proof_size(), rldp_);
            } else {
              td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_proof",
                                      create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
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
                td::actor::send_closure(SelfId, &DownloadBlock::abort_query, R.move_as_error());
              } else {
                td::actor::send_closure(SelfId, &DownloadBlock::got_block_partial_proof, R.move_as_ok());
              }
            });

            auto q = create_serialize_tl_object<ton_api::tonNode_downloadBlockProofLink>(create_tl_block_id(block_id_));
            if (client_.empty()) {
              td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_,
                                      overlay_id_, "get_proof_link", std::move(P), td::Timestamp::in(3.0), std::move(q),
                                      FullNode::max_proof_size(), rldp_);
            } else {
              td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_proof_link",
                                      create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                                      td::Timestamp::in(3.0), std::move(P));
            }
          },
          [&](ton_api::tonNode_preparedProofEmpty &obj) {
            abort_query(td::Status::Error(ErrorCode::notready, "proof not found"));
          }));
}

void DownloadBlock::got_block_proof(td::BufferSlice proof) {
  VLOG(FULL_NODE_DEBUG) << "downloaded proof for " << block_id_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadBlock::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadBlock::checked_block_proof);
    }
  });

  if (!prev_) {
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_proof, block_id_,
                            std::move(proof), std::move(P));
  } else {
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_is_next_proof, prev_->id(),
                            block_id_, std::move(proof), std::move(P));
  }
}

void DownloadBlock::got_block_partial_proof(td::BufferSlice proof) {
  CHECK(allow_partial_proof_);
  CHECK(!prev_);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadBlock::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadBlock::checked_block_proof);
    }
  });

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::validate_block_proof_link, block_id_,
                          std::move(proof), std::move(P));
}

void DownloadBlock::checked_block_proof() {
  VLOG(FULL_NODE_DEBUG) << "checked proof for " << block_id_;

  if (!handle_) {
    CHECK(!short_);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadBlock::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &DownloadBlock::got_block_handle_2, R.move_as_ok());
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle, block_id_, true,
                            std::move(P));
  } else {
    got_block_handle_2(handle_);
  }
}

void DownloadBlock::got_block_handle_2(BlockHandle handle) {
  handle_ = std::move(handle);
  LOG_CHECK(skip_proof_ || handle_->inited_proof() || (allow_partial_proof_ && handle_->inited_proof_link()))
      << handle_->id() << " allowpartial=" << allow_partial_proof_;

  if (handle_->received()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadBlock::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &DownloadBlock::got_block_data, R.move_as_ok());
      }
    });

    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_data, handle_, std::move(P));
  } else {
    CHECK(!short_);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadBlock::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &DownloadBlock::got_block_data_description, R.move_as_ok());
      }
    });

    auto q = create_serialize_tl_object<ton_api::tonNode_prepareBlock>(create_tl_block_id(block_id_));
    if (client_.empty()) {
      td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                              "get_prepare_block", std::move(P), td::Timestamp::in(1.0), std::move(q));
    } else {
      td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_prepare_block",
                              create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                              td::Timestamp::in(1.0), std::move(P));
    }
  }
}

void DownloadBlock::got_block_data_description(td::BufferSlice data_description) {
  VLOG(FULL_NODE_DEBUG) << "downloaded data description for " << block_id_;
  auto F = fetch_tl_object<ton_api::tonNode_Prepared>(std::move(data_description), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();

  ton_api::downcast_call(
      *f.get(),
      td::overloaded(
          [&, self = this](ton_api::tonNode_prepared &val) {
            auto P = td::PromiseCreator::lambda([SelfId = actor_id(self)](td::Result<td::BufferSlice> R) {
              if (R.is_error()) {
                td::actor::send_closure(SelfId, &DownloadBlock::abort_query, R.move_as_error());
              } else {
                td::actor::send_closure(SelfId, &DownloadBlock::got_block_data, R.move_as_ok());
              }
            });

            auto q = create_serialize_tl_object<ton_api::tonNode_downloadBlock>(create_tl_block_id(block_id_));
            if (client_.empty()) {
              td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_,
                                      overlay_id_, "get_block", std::move(P), td::Timestamp::in(15.0), std::move(q),
                                      FullNode::max_block_size(), rldp_);
            } else {
              td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_block",
                                      create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                                      td::Timestamp::in(15.0), std::move(P));
            }
          },
          [&](ton_api::tonNode_notFound &val) {
            abort_query(td::Status::Error(ErrorCode::notready, "dst node does not have block"));
          }));
}

void DownloadBlock::got_block_data(td::BufferSlice data) {
  VLOG(FULL_NODE_DEBUG) << "downloaded data for " << block_id_;
  block_.data = std::move(data);
  finish_query();
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
