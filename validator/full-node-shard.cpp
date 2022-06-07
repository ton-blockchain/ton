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
#include "auto/tl/ton_api.h"
#include "overlays.h"
#include "td/utils/SharedSlice.h"
#include "full-node-shard.hpp"
#include "full-node-shard-queries.hpp"

#include "ton/ton-shard.h"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"

#include "adnl/utils.hpp"
#include "net/download-block-new.hpp"
#include "net/download-block.hpp"
#include "net/download-next-block.hpp"
#include "net/download-state.hpp"
#include "net/download-proof.hpp"
#include "net/get-next-key-blocks.hpp"
#include "net/download-archive-slice.hpp"

#include "td/utils/Random.h"

#include "common/delay.h"

namespace ton {

namespace validator {

namespace fullnode {

Neighbour Neighbour::zero = Neighbour{adnl::AdnlNodeIdShort::zero()};

void Neighbour::update_proto_version(const ton_api::tonNode_capabilities &q) {
  proto_version = q.version_;
  capabilities = q.capabilities_;
}

void Neighbour::query_success(double t) {
  unreliability--;
  if (unreliability < 0) {
    unreliability = 0;
  }
  update_roundtrip(t);
}

void Neighbour::query_failed() {
  unreliability++;
}

void Neighbour::update_roundtrip(double t) {
  roundtrip = (t + roundtrip) * 0.5;
}

void FullNodeShardImpl::create_overlay() {
  class Callback : public overlay::Overlays::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
      // just ignore
    }
    void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(node_, &FullNodeShardImpl::receive_query, src, std::move(data), std::move(promise));
    }
    void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
      td::actor::send_closure(node_, &FullNodeShardImpl::receive_broadcast, src, std::move(data));
    }
    void check_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                         td::Promise<td::Unit> promise) override {
      td::actor::send_closure(node_, &FullNodeShardImpl::check_broadcast, src, std::move(data), std::move(promise));
    }
    Callback(td::actor::ActorId<FullNodeShardImpl> node) : node_(node) {
    }

   private:
    td::actor::ActorId<FullNodeShardImpl> node_;
  };

  td::actor::send_closure(overlays_, &overlay::Overlays::create_public_overlay, adnl_id_, overlay_id_full_.clone(),
                          std::make_unique<Callback>(actor_id(this)), rules_, PSTRING() << "{ \"type\": \"shard\", \"shard_id\": " << get_shard() << ", \"workchain_id\": " << get_workchain() << " }");

  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, adnl_id_);
  if (cert_) {
    td::actor::send_closure(overlays_, &overlay::Overlays::update_certificate, adnl_id_, overlay_id_, local_id_, cert_);
  }
}

void FullNodeShardImpl::check_broadcast(PublicKeyHash src, td::BufferSlice broadcast, td::Promise<td::Unit> promise) {
  auto B = fetch_tl_object<ton_api::tonNode_externalMessageBroadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    return promise.set_error(B.move_as_error_prefix("failed to parse external message broadcast: "));
  }

  auto q = B.move_as_ok();
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::check_external_message,
                          std::move(q->message_->data_), std::move(promise));
}

void FullNodeShardImpl::update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) {
  td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, adnl_id_, overlay_id_);
  adnl_id_ = adnl_id;
  local_id_ = adnl_id_.pubkey_hash();
  create_overlay();
}

void FullNodeShardImpl::try_get_next_block(td::Timestamp timeout, td::Promise<ReceivedBlock> promise) {
  if (timeout.is_in_past()) {
    promise.set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
    return;
  }

  auto &b = choose_neighbour();
  if (!b.adnl_id.is_zero() && b.proto_version >= 1) {
    VLOG(FULL_NODE_DEBUG) << "using new download method with adnlid=" << b.adnl_id;
    td::actor::create_actor<DownloadBlockNew>("downloadnext", adnl_id_, overlay_id_, handle_->id(), b.adnl_id,
                                              download_next_priority(), timeout, validator_manager_, rldp_, overlays_,
                                              adnl_, client_, create_neighbour_promise(b, std::move(promise)))
        .release();
  } else {
    VLOG(FULL_NODE_DEBUG) << "using old download method with adnlid=" << b.adnl_id;
    td::actor::create_actor<DownloadNextBlock>("downloadnext", adnl_id_, overlay_id_, handle_, b.adnl_id,
                                               download_next_priority(), timeout, validator_manager_, rldp_, overlays_,
                                               adnl_, client_, create_neighbour_promise(b, std::move(promise)))
        .release();
  }
}

void FullNodeShardImpl::got_next_block(td::Result<BlockHandle> R) {
  if (R.is_error()) {
    if (R.error().code() == ErrorCode::timeout || R.error().code() == ErrorCode::notready) {
      get_next_block();
      return;
    }
  }
  attempt_ = 0;
  R.ensure();
  auto old_seqno = handle_->id().id.seqno;
  handle_ = R.move_as_ok();
  CHECK(handle_->id().id.seqno == old_seqno + 1);

  if (promise_) {
    if (handle_->unix_time() > td::Clocks::system() - 300) {
      promise_.set_value(td::Unit());
    } else {
      sync_completed_at_ = td::Timestamp::in(60.0);
    }
  }
  get_next_block();
}

void FullNodeShardImpl::get_next_block() {
  //return;
  attempt_++;
  auto P = td::PromiseCreator::lambda([validator_manager = validator_manager_, attempt = attempt_,
                                       block_id = handle_->id(), SelfId = actor_id(this)](td::Result<ReceivedBlock> R) {
    if (R.is_ok()) {
      auto P = td::PromiseCreator::lambda([SelfId](td::Result<BlockHandle> R) {
        td::actor::send_closure(SelfId, &FullNodeShardImpl::got_next_block, std::move(R));
      });
      td::actor::send_closure(validator_manager, &ValidatorManagerInterface::validate_block, R.move_as_ok(),
                              std::move(P));
    } else {
      auto S = R.move_as_error();
      if (S.code() != ErrorCode::notready && S.code() != ErrorCode::timeout) {
        VLOG(FULL_NODE_WARNING) << "failed to download next block after " << block_id << ": " << S;
      } else {
        if ((attempt % 128) == 0) {
          VLOG(FULL_NODE_INFO) << "failed to download next block after " << block_id << ": " << S;
        } else {
          VLOG(FULL_NODE_DEBUG) << "failed to download next block after " << block_id << ": " << S;
        }
      }
      delay_action([SelfId]() mutable { td::actor::send_closure(SelfId, &FullNodeShardImpl::get_next_block); },
                   td::Timestamp::in(0.1));
    }
  });
  try_get_next_block(td::Timestamp::in(2.0), std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getNextBlockDescription &query,
                                      td::Promise<td::BufferSlice> promise) {
  if (query.prev_block_->workchain_ != masterchainId || static_cast<ShardId>(query.prev_block_->shard_) != shardIdAll) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "next block allowed only for masterchain"));
    return;
  }
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      auto x = create_serialize_tl_object<ton_api::tonNode_blockDescriptionEmpty>();
      promise.set_value(std::move(x));
    } else {
      auto B = R.move_as_ok();
      if (!B->received() || !B->inited_proof()) {
        auto x = create_serialize_tl_object<ton_api::tonNode_blockDescriptionEmpty>();
        promise.set_value(std::move(x));
      } else {
        auto x = create_serialize_tl_object<ton_api::tonNode_blockDescription>(create_tl_block_id(B->id()));
        promise.set_value(std::move(x));
      }
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_next_block,
                          create_block_id(query.prev_block_), std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareBlock &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      auto x = create_serialize_tl_object<ton_api::tonNode_notFound>();
      promise.set_value(std::move(x));
    } else {
      auto B = R.move_as_ok();
      if (!B->received()) {
        auto x = create_serialize_tl_object<ton_api::tonNode_notFound>();
        promise.set_value(std::move(x));
      } else {
        auto x = create_serialize_tl_object<ton_api::tonNode_prepared>();
        promise.set_value(std::move(x));
      }
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle,
                          create_block_id(query.block_), false, std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlock &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([validator_manager = validator_manager_,
                                       promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block"));
    } else {
      auto B = R.move_as_ok();
      if (!B->received()) {
        promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block"));
      } else {
        td::actor::send_closure(validator_manager, &ValidatorManagerInterface::get_block_data, B, std::move(promise));
      }
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle,
                          create_block_id(query.block_), false, std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockFull &query,
                                      td::Promise<td::BufferSlice> promise) {
  td::actor::create_actor<BlockFullSender>("sender", ton::create_block_id(query.block_), false, validator_manager_,
                                           std::move(promise))
      .release();
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadNextBlockFull &query,
                                      td::Promise<td::BufferSlice> promise) {
  td::actor::create_actor<BlockFullSender>("sender", ton::create_block_id(query.prev_block_), true, validator_manager_,
                                           std::move(promise))
      .release();
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareBlockProof &query,
                                      td::Promise<td::BufferSlice> promise) {
  if (query.block_->seqno_ == 0) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "cannot download proof for zero state"));
    return;
  }
  auto P = td::PromiseCreator::lambda([allow_partial = query.allow_partial_, promise = std::move(promise),
                                       validator_manager = validator_manager_](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      auto x = create_serialize_tl_object<ton_api::tonNode_preparedProofEmpty>();
      promise.set_value(std::move(x));
      return;
    } else {
      auto handle = R.move_as_ok();
      if (!handle || (!handle->inited_proof() && (!allow_partial || !handle->inited_proof_link()))) {
        auto x = create_serialize_tl_object<ton_api::tonNode_preparedProofEmpty>();
        promise.set_value(std::move(x));
        return;
      }
      if (handle->inited_proof() && handle->id().is_masterchain()) {
        auto x = create_serialize_tl_object<ton_api::tonNode_preparedProof>();
        promise.set_value(std::move(x));
      } else {
        auto x = create_serialize_tl_object<ton_api::tonNode_preparedProofLink>();
        promise.set_value(std::move(x));
      }
    }
  });

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle,
                          create_block_id(query.block_), false, std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareKeyBlockProof &query,
                                      td::Promise<td::BufferSlice> promise) {
  if (query.block_->seqno_ == 0) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "cannot download proof for zero state"));
    return;
  }
  auto P = td::PromiseCreator::lambda(
      [allow_partial = query.allow_partial_, promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
        if (R.is_error()) {
          auto x = create_serialize_tl_object<ton_api::tonNode_preparedProofEmpty>();
          promise.set_value(std::move(x));
        } else if (allow_partial) {
          auto x = create_serialize_tl_object<ton_api::tonNode_preparedProofLink>();
          promise.set_value(std::move(x));
        } else {
          auto x = create_serialize_tl_object<ton_api::tonNode_preparedProof>();
          promise.set_value(std::move(x));
        }
      });

  if (query.allow_partial_) {
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_key_block_proof_link,
                            create_block_id(query.block_), std::move(P));
  } else {
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_key_block_proof,
                            create_block_id(query.block_), std::move(P));
  }
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockProof &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), validator_manager = validator_manager_](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
          return;
        } else {
          auto handle = R.move_as_ok();
          if (!handle || !handle->inited_proof()) {
            promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
            return;
          }

          td::actor::send_closure(validator_manager, &ValidatorManagerInterface::get_block_proof, handle,
                                  std::move(promise));
        }
      });

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle,
                          create_block_id(query.block_), false, std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockProofLink &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), validator_manager = validator_manager_](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
          return;
        } else {
          auto handle = R.move_as_ok();
          if (!handle || !handle->inited_proof_link()) {
            promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
            return;
          }

          td::actor::send_closure(validator_manager, &ValidatorManagerInterface::get_block_proof_link, handle,
                                  std::move(promise));
        }
      });

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_handle,
                          create_block_id(query.block_), false, std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadKeyBlockProof &query,
                                      td::Promise<td::BufferSlice> promise) {
  if (query.block_->seqno_ == 0) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "cannot download proof for zero state"));
    return;
  }
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
    } else {
      promise.set_value(R.move_as_ok());
    }
  });

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_key_block_proof,
                          create_block_id(query.block_), std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadKeyBlockProofLink &query,
                                      td::Promise<td::BufferSlice> promise) {
  if (query.block_->seqno_ == 0) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "cannot download proof for zero state"));
    return;
  }
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(td::Status::Error(ErrorCode::protoviolation, "unknown block proof"));
    } else {
      promise.set_value(R.move_as_ok());
    }
  });

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_key_block_proof_link,
                          create_block_id(query.block_), std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareZeroState &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this), promise = std::move(promise)](td::Result<bool> R) mutable {
        if (R.is_error() || !R.move_as_ok()) {
          auto x = create_serialize_tl_object<ton_api::tonNode_notFoundState>();
          promise.set_value(std::move(x));
          return;
        }

        auto x = create_serialize_tl_object<ton_api::tonNode_preparedState>();
        promise.set_value(std::move(x));
      });
  auto block_id = create_block_id(query.block_);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::check_zero_state_exists, block_id,
                          std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_preparePersistentState &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this), promise = std::move(promise)](td::Result<bool> R) mutable {
        if (R.is_error() || !R.move_as_ok()) {
          auto x = create_serialize_tl_object<ton_api::tonNode_notFoundState>();
          promise.set_value(std::move(x));
          return;
        }

        auto x = create_serialize_tl_object<ton_api::tonNode_preparedState>();
        promise.set_value(std::move(x));
      });
  auto block_id = create_block_id(query.block_);
  auto masterchain_block_id = create_block_id(query.masterchain_block_);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::check_persistent_state_exists, block_id,
                          masterchain_block_id, std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getNextKeyBlockIds &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto cnt = static_cast<td::uint32>(query.max_size_);
  if (cnt > 8) {
    cnt = 8;
  }
  auto P =
      td::PromiseCreator::lambda([promise = std::move(promise), cnt](td::Result<std::vector<BlockIdExt>> R) mutable {
        if (R.is_error()) {
          LOG(WARNING) << "getnextkey: " << R.move_as_error();
          auto x = create_serialize_tl_object<ton_api::tonNode_keyBlocks>(
              std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>>{}, false, true);
          promise.set_value(std::move(x));
          return;
        }
        auto res = R.move_as_ok();
        std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>> v;
        for (auto &b : res) {
          v.emplace_back(create_tl_block_id(b));
        }
        auto x = create_serialize_tl_object<ton_api::tonNode_keyBlocks>(std::move(v), res.size() < cnt, false);
        promise.set_value(std::move(x));
      });
  auto block_id = create_block_id(query.block_);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_next_key_blocks, block_id, cnt,
                          std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadZeroState &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error_prefix("failed to get state from db: "));
          return;
        }

        promise.set_value(R.move_as_ok());
      });
  auto block_id = create_block_id(query.block_);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_zero_state, block_id, std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadPersistentState &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error_prefix("failed to get state from db: "));
          return;
        }

        promise.set_value(R.move_as_ok());
      });
  auto block_id = create_block_id(query.block_);
  auto masterchain_block_id = create_block_id(query.masterchain_block_);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_persistent_state, block_id,
                          masterchain_block_id, std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadPersistentStateSlice &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error_prefix("failed to get state from db: "));
          return;
        }

        promise.set_value(R.move_as_ok());
      });
  auto block_id = create_block_id(query.block_);
  auto masterchain_block_id = create_block_id(query.masterchain_block_);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_persistent_state_slice, block_id,
                          masterchain_block_id, query.offset_, query.max_size_, std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getCapabilities &query,
                                      td::Promise<td::BufferSlice> promise) {
  promise.set_value(create_serialize_tl_object<ton_api::tonNode_capabilities>(proto_version(), proto_capabilities()));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getArchiveInfo &query,
                                      td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::uint64> R) mutable {
        if (R.is_error()) {
          promise.set_value(create_serialize_tl_object<ton_api::tonNode_archiveNotFound>());
        } else {
          promise.set_value(create_serialize_tl_object<ton_api::tonNode_archiveInfo>(R.move_as_ok()));
        }
      });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_archive_id, query.masterchain_seqno_,
                          std::move(P));
}

void FullNodeShardImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getArchiveSlice &query,
                                      td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_archive_slice, query.archive_id_,
                          query.offset_, query.max_size_, std::move(promise));
}

void FullNodeShardImpl::receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query,
                                      td::Promise<td::BufferSlice> promise) {
  auto B = fetch_tl_object<ton_api::Function>(std::move(query), true);
  if (B.is_error()) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "cannot parse tonnode query"));
    return;
  }
  ton_api::downcast_call(*B.move_as_ok().get(), [&](auto &obj) { this->process_query(src, obj, std::move(promise)); });
}

void FullNodeShardImpl::process_broadcast(PublicKeyHash src, ton_api::tonNode_ihrMessageBroadcast &query) {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_ihr_message,
                          std::move(query.message_->data_));
}

void FullNodeShardImpl::process_broadcast(PublicKeyHash src, ton_api::tonNode_externalMessageBroadcast &query) {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_external_message,
                          std::move(query.message_->data_));
}

void FullNodeShardImpl::process_broadcast(PublicKeyHash src, ton_api::tonNode_newShardBlockBroadcast &query) {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_shard_block,
                          create_block_id(query.block_->block_), query.block_->cc_seqno_,
                          std::move(query.block_->data_));
}

void FullNodeShardImpl::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockBroadcast &query) {
  std::vector<BlockSignature> signatures;
  for (auto &sig : query.signatures_) {
    signatures.emplace_back(BlockSignature{sig->who_, std::move(sig->signature_)});
  }

  BlockIdExt block_id = create_block_id(query.id_);
  BlockBroadcast B{block_id,
                   std::move(signatures),
                   static_cast<UnixTime>(query.catchain_seqno_),
                   static_cast<td::uint32>(query.validator_set_hash_),
                   std::move(query.data_),
                   std::move(query.proof_)};

  auto P = td::PromiseCreator::lambda([](td::Result<td::Unit> R) {
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::notready) {
        LOG(DEBUG) << "dropped broadcast: " << R.move_as_error();
      } else {
        LOG(INFO) << "dropped broadcast: " << R.move_as_error();
      }
    }
  });
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::prevalidate_block, std::move(B),
                          std::move(P));
}

void FullNodeShardImpl::receive_broadcast(PublicKeyHash src, td::BufferSlice broadcast) {
  auto B = fetch_tl_object<ton_api::tonNode_Broadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    return;
  }

  ton_api::downcast_call(*B.move_as_ok().get(), [src, Self = this](auto &obj) { Self->process_broadcast(src, obj); });
}

void FullNodeShardImpl::send_ihr_message(td::BufferSlice data) {
  if (!client_.empty()) {
    UNREACHABLE();
    return;
  }
  auto B = create_serialize_tl_object<ton_api::tonNode_ihrMessageBroadcast>(
      create_tl_object<ton_api::tonNode_ihrMessage>(std::move(data)));
  if (B.size() <= overlay::Overlays::max_simple_broadcast_size()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_ex, adnl_id_, overlay_id_, local_id_, 0,
                            std::move(B));
  } else {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, adnl_id_, overlay_id_, local_id_, 0,
                            std::move(B));
  }
}

void FullNodeShardImpl::send_external_message(td::BufferSlice data) {
  if (!client_.empty()) {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "send_ext_query",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(
                                create_serialize_tl_object<ton_api::tonNode_slave_sendExtMessage>(
                                    create_tl_object<ton_api::tonNode_externalMessage>(std::move(data)))),
                            td::Timestamp::in(1.0), [](td::Result<td::BufferSlice> R) {
                              if (R.is_error()) {
                                VLOG(FULL_NODE_WARNING) << "failed to send ext message: " << R.move_as_error();
                              }
                            });
    return;
  }
  auto B = create_serialize_tl_object<ton_api::tonNode_externalMessageBroadcast>(
      create_tl_object<ton_api::tonNode_externalMessage>(std::move(data)));
  if (B.size() <= overlay::Overlays::max_simple_broadcast_size()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_ex, adnl_id_, overlay_id_, local_id_, 0,
                            std::move(B));
  } else {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, adnl_id_, overlay_id_, local_id_, 0,
                            std::move(B));
  }
}

void FullNodeShardImpl::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  if (!client_.empty()) {
    UNREACHABLE();
    return;
  }
  auto B = create_serialize_tl_object<ton_api::tonNode_newShardBlockBroadcast>(
      create_tl_object<ton_api::tonNode_newShardBlock>(create_tl_block_id(block_id), cc_seqno, std::move(data)));
  if (B.size() <= overlay::Overlays::max_simple_broadcast_size()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_ex, adnl_id_, overlay_id_, local_id_, 0,
                            std::move(B));
  } else {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, adnl_id_, overlay_id_, local_id_,
                            overlay::Overlays::BroadcastFlagAnySender(), std::move(B));
  }
}

void FullNodeShardImpl::send_broadcast(BlockBroadcast broadcast) {
  if (!client_.empty()) {
    UNREACHABLE();
    return;
  }
  std::vector<tl_object_ptr<ton_api::tonNode_blockSignature>> sigs;
  for (auto &sig : broadcast.signatures) {
    sigs.emplace_back(create_tl_object<ton_api::tonNode_blockSignature>(sig.node, sig.signature.clone()));
  }
  auto B = create_serialize_tl_object<ton_api::tonNode_blockBroadcast>(
      create_tl_block_id(broadcast.block_id), broadcast.catchain_seqno, broadcast.validator_set_hash, std::move(sigs),
      broadcast.proof.clone(), broadcast.data.clone());
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, adnl_id_, overlay_id_, local_id_,
                          overlay::Overlays::BroadcastFlagAnySender(), std::move(B));
}

void FullNodeShardImpl::download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                       td::Promise<ReceivedBlock> promise) {
  auto &b = choose_neighbour();
  if (!b.adnl_id.is_zero() && b.proto_version >= 1) {
    VLOG(FULL_NODE_DEBUG) << "new block download";
    td::actor::create_actor<DownloadBlockNew>("downloadreq", id, adnl_id_, overlay_id_, b.adnl_id, priority, timeout,
                                              validator_manager_, rldp_, overlays_, adnl_, client_,
                                              create_neighbour_promise(b, std::move(promise)))
        .release();
  } else {
    VLOG(FULL_NODE_DEBUG) << "old block download";
    td::actor::create_actor<DownloadBlock>("downloadreq", id, adnl_id_, overlay_id_, b.adnl_id, priority, timeout,
                                           validator_manager_, rldp_, overlays_, adnl_, client_,
                                           create_neighbour_promise(b, std::move(promise)))
        .release();
  }
}

void FullNodeShardImpl::download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                            td::Promise<td::BufferSlice> promise) {
  td::actor::create_actor<DownloadState>(PSTRING() << "downloadstatereq" << id.id.to_str(), id, BlockIdExt{}, adnl_id_,
                                         overlay_id_, adnl::AdnlNodeIdShort::zero(), priority, timeout,
                                         validator_manager_, rldp_, overlays_, adnl_, client_, std::move(promise))
      .release();
}

void FullNodeShardImpl::download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                                  td::Timestamp timeout, td::Promise<td::BufferSlice> promise) {
  td::actor::create_actor<DownloadState>(PSTRING() << "downloadstatereq" << id.id.to_str(), id, masterchain_block_id,
                                         adnl_id_, overlay_id_, adnl::AdnlNodeIdShort::zero(), priority, timeout,
                                         validator_manager_, rldp_, overlays_, adnl_, client_, std::move(promise))
      .release();
}

void FullNodeShardImpl::download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                             td::Promise<td::BufferSlice> promise) {
  auto &b = choose_neighbour();
  td::actor::create_actor<DownloadProof>("downloadproofreq", block_id, false, false, adnl_id_, overlay_id_, b.adnl_id,
                                         priority, timeout, validator_manager_, rldp_, overlays_, adnl_, client_,
                                         create_neighbour_promise(b, std::move(promise)))
      .release();
}

void FullNodeShardImpl::download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                                  td::Promise<td::BufferSlice> promise) {
  auto &b = choose_neighbour();
  td::actor::create_actor<DownloadProof>("downloadproofreq", block_id, true, false, adnl_id_, overlay_id_,
                                         adnl::AdnlNodeIdShort::zero(), priority, timeout, validator_manager_, rldp_,
                                         overlays_, adnl_, client_, create_neighbour_promise(b, std::move(promise)))
      .release();
}

void FullNodeShardImpl::get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                                            td::Promise<std::vector<BlockIdExt>> promise) {
  auto &b = choose_neighbour();
  td::actor::create_actor<GetNextKeyBlocks>("next", block_id, 16, adnl_id_, overlay_id_, adnl::AdnlNodeIdShort::zero(),
                                            1, timeout, validator_manager_, rldp_, overlays_, adnl_, client_,
                                            create_neighbour_promise(b, std::move(promise)))
      .release();
}

void FullNodeShardImpl::download_archive(BlockSeqno masterchain_seqno, std::string tmp_dir, td::Timestamp timeout,
                                         td::Promise<std::string> promise) {
  auto &b = choose_neighbour();
  td::actor::create_actor<DownloadArchiveSlice>(
      "archive", masterchain_seqno, std::move(tmp_dir), adnl_id_, overlay_id_, adnl::AdnlNodeIdShort::zero(), timeout,
      validator_manager_, rldp_, overlays_, adnl_, client_, create_neighbour_promise(b, std::move(promise)))
      .release();
}

void FullNodeShardImpl::set_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  CHECK(!handle_);
  handle_ = std::move(handle);
  promise_ = std::move(promise);
  get_next_block();

  sync_completed_at_ = td::Timestamp::in(60.0);
  alarm_timestamp().relax(sync_completed_at_);
}

void FullNodeShardImpl::alarm() {
  if (sync_completed_at_ && sync_completed_at_.is_in_past()) {
    if (promise_) {
      promise_.set_value(td::Unit());
    }
    sync_completed_at_ = td::Timestamp::never();
  }
  if (reload_neighbours_at_ && reload_neighbours_at_.is_in_past()) {
    reload_neighbours();
    reload_neighbours_at_ = td::Timestamp::in(td::Random::fast(10.0, 30.0));
  }
  if (ping_neighbours_at_ && ping_neighbours_at_.is_in_past()) {
    ping_neighbours();
    ping_neighbours_at_ = td::Timestamp::in(td::Random::fast(0.5, 1.0));
  }
  if (update_certificate_at_ && update_certificate_at_.is_in_past()) {
    if (!sign_cert_by_.is_zero()) {
      sign_new_certificate(sign_cert_by_);
      update_certificate_at_ = td::Timestamp::in(30.0);
    } else {
      update_certificate_at_ = td::Timestamp::never();
    }
  }
  alarm_timestamp().relax(sync_completed_at_);
  alarm_timestamp().relax(update_certificate_at_);
  alarm_timestamp().relax(reload_neighbours_at_);
  alarm_timestamp().relax(ping_neighbours_at_);
}

void FullNodeShardImpl::start_up() {
  if (client_.empty()) {
    auto X = create_hash_tl_object<ton_api::tonNode_shardPublicOverlayId>(get_workchain(), get_shard(),
                                                                          zero_state_file_hash_);
    td::BufferSlice b{32};
    b.as_slice().copy_from(as_slice(X));
    overlay_id_full_ = overlay::OverlayIdFull{std::move(b)};
    overlay_id_ = overlay_id_full_.compute_short_id();
    rules_ = overlay::OverlayPrivacyRules{overlay::Overlays::max_fec_broadcast_size()};

    create_overlay();

    reload_neighbours_at_ = td::Timestamp::now();
    ping_neighbours_at_ = td::Timestamp::now();
    alarm_timestamp().relax(reload_neighbours_at_);
    alarm_timestamp().relax(ping_neighbours_at_);
  }
}

void FullNodeShardImpl::sign_new_certificate(PublicKeyHash sign_by) {
  if (sign_by.is_zero()) {
    return;
  }

  ton::overlay::Certificate cert{
      sign_by, static_cast<td::int32>(td::Clocks::system() + 3600), overlay::Overlays::max_fec_broadcast_size(),
      overlay::CertificateFlags::Trusted | overlay::CertificateFlags::AllowFec, td::BufferSlice{}};
  auto to_sign = cert.to_sign(overlay_id_, local_id_);

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), cert = std::move(cert)](td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
        if (R.is_error()) {
          // ignore
          VLOG(FULL_NODE_WARNING) << "failed to create certificate: failed to sign: " << R.move_as_error();
        } else {
          auto p = R.move_as_ok();
          cert.set_signature(std::move(p.first));
          cert.set_issuer(p.second);
          td::actor::send_closure(SelfId, &FullNodeShardImpl::signed_new_certificate, std::move(cert));
        }
      });
  td::actor::send_closure(keyring_, &ton::keyring::Keyring::sign_add_get_public_key, sign_by, std::move(to_sign),
                          std::move(P));
}

void FullNodeShardImpl::signed_new_certificate(ton::overlay::Certificate cert) {
  LOG(WARNING) << "updated certificate";
  cert_ = std::make_shared<overlay::Certificate>(std::move(cert));
  td::actor::send_closure(overlays_, &overlay::Overlays::update_certificate, adnl_id_, overlay_id_, local_id_, cert_);
}

void FullNodeShardImpl::sign_overlay_certificate(PublicKeyHash signed_key, td::uint32 expire_at, td::uint32 max_size, td::Promise<td::BufferSlice> promise) {
  auto sign_by = sign_cert_by_;
  if (sign_by.is_zero()) {
    promise.set_error(td::Status::Error("Node has no key with signing authority"));
    return;
  }

  ton::overlay::Certificate cert{
      sign_by, static_cast<td::int32>(expire_at), max_size,
      overlay::CertificateFlags::Trusted | overlay::CertificateFlags::AllowFec, td::BufferSlice{}};
  auto to_sign = cert.to_sign(overlay_id_, signed_key);

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), expire_at = expire_at, max_size = max_size, promise = std::move(promise)](td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error_prefix("failed to create certificate: failed to sign: "));
        } else {
          auto p = R.move_as_ok();
          auto c = ton::create_serialize_tl_object<ton::ton_api::overlay_certificate>(p.second.tl(), static_cast<td::int32>(expire_at), max_size, std::move(p.first));
          promise.set_value(std::move(c));
        }
      });
  td::actor::send_closure(keyring_, &ton::keyring::Keyring::sign_add_get_public_key, sign_by, std::move(to_sign),
                          std::move(P));
}

void FullNodeShardImpl::import_overlay_certificate(PublicKeyHash signed_key, std::shared_ptr<ton::overlay::Certificate> cert, td::Promise<td::Unit> promise) {
  td::actor::send_closure(overlays_, &ton::overlay::Overlays::update_certificate,
                                     adnl_id_, overlay_id_, signed_key, cert);
  promise.set_value( td::Unit()  );
}

void FullNodeShardImpl::update_validators(std::vector<PublicKeyHash> public_key_hashes, PublicKeyHash local_hash) {
  if (!client_.empty()) {
    return;
  }
  bool update_cert = false;
  if (!local_hash.is_zero() && local_hash != sign_cert_by_) {
    update_cert = true;
  }
  sign_cert_by_ = local_hash;

  std::map<PublicKeyHash, td::uint32> authorized_keys;
  for (auto &key : public_key_hashes) {
    authorized_keys.emplace(key, overlay::Overlays::max_fec_broadcast_size());
  }

  rules_ = overlay::OverlayPrivacyRules{overlay::Overlays::max_fec_broadcast_size(), overlay::CertificateFlags::AllowFec, std::move(authorized_keys)};
  td::actor::send_closure(overlays_, &overlay::Overlays::set_privacy_rules, adnl_id_, overlay_id_, rules_);

  if (update_cert) {
    sign_new_certificate(sign_cert_by_);
    update_certificate_at_ = td::Timestamp::in(30.0);
    alarm_timestamp().relax(update_certificate_at_);
  }
}

void FullNodeShardImpl::reload_neighbours() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
    if (R.is_error()) {
      return;
    }
    auto vec = R.move_as_ok();
    if (vec.size() == 0) {
      return;
    } else {
      td::actor::send_closure(SelfId, &FullNodeShardImpl::got_neighbours, std::move(vec));
    }
  });
  td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, adnl_id_, overlay_id_,
                          max_neighbours(), std::move(P));
}

void FullNodeShardImpl::got_neighbours(std::vector<adnl::AdnlNodeIdShort> vec) {
  bool ex = false;

  for (auto &el : vec) {
    auto it = neighbours_.find(el);
    if (it != neighbours_.end()) {
      continue;
    }
    if (neighbours_.size() == max_neighbours()) {
      adnl::AdnlNodeIdShort a = adnl::AdnlNodeIdShort::zero();
      adnl::AdnlNodeIdShort b = adnl::AdnlNodeIdShort::zero();
      td::uint32 cnt = 0;
      double u = 0;
      for (auto &n : neighbours_) {
        if (n.second.unreliability > u) {
          u = n.second.unreliability;
          a = n.first;
        }
        if (td::Random::fast(0, cnt++) == 0) {
          b = n.first;
        }
      }

      if (u > stop_unreliability()) {
        neighbours_.erase(a);
      } else {
        neighbours_.erase(b);
        ex = true;
      }
    }
    neighbours_.emplace(el, Neighbour{el});
    if (ex) {
      break;
    }
  }
}

const Neighbour &FullNodeShardImpl::choose_neighbour() const {
  if (neighbours_.size() == 0) {
    return Neighbour::zero;
  }

  const Neighbour *best = nullptr;
  td::uint32 sum = 0;

  for (auto &x : neighbours_) {
    td::uint32 unr = static_cast<td::uint32>(x.second.unreliability);

    if (x.second.proto_version < proto_version()) {
      unr += 4;
    } else if (x.second.proto_version == proto_version() && x.second.capabilities < proto_capabilities()) {
      unr += 2;
    }

    auto f = static_cast<td::uint32>(fail_unreliability());

    if (unr <= f) {
      auto w = 1 << (f - unr);
      sum += w;
      if (td::Random::fast(0, sum - 1) <= w - 1) {
        best = &x.second;
      }
    }
  }
  return best ? *best : Neighbour::zero;
}

void FullNodeShardImpl::update_neighbour_stats(adnl::AdnlNodeIdShort adnl_id, double t, bool success) {
  auto it = neighbours_.find(adnl_id);
  if (it != neighbours_.end()) {
    if (success) {
      it->second.query_success(t);
    } else {
      it->second.query_failed();
    }
  }
}

void FullNodeShardImpl::got_neighbour_capabilities(adnl::AdnlNodeIdShort adnl_id, double t, td::BufferSlice data) {
  auto it = neighbours_.find(adnl_id);
  if (it == neighbours_.end()) {
    return;
  }
  auto F = fetch_tl_object<ton_api::tonNode_capabilities>(std::move(data), true);
  if (F.is_error()) {
    it->second.query_failed();
  } else {
    it->second.update_proto_version(*F.move_as_ok().get());
    it->second.query_success(t);
  }
}

void FullNodeShardImpl::ping_neighbours() {
  if (neighbours_.size() == 0) {
    return;
  }
  td::uint32 max_cnt = 6;
  if (max_cnt > neighbours_.size()) {
    max_cnt = td::narrow_cast<td::uint32>(neighbours_.size());
  }
  auto it = neighbours_.lower_bound(last_pinged_neighbour_);
  while (max_cnt > 0) {
    if (it == neighbours_.end()) {
      it = neighbours_.begin();
    }

    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), start_time = td::Time::now(), id = it->first](td::Result<td::BufferSlice> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &FullNodeShardImpl::update_neighbour_stats, id,
                                    td::Time::now() - start_time, false);
          } else {
            td::actor::send_closure(SelfId, &FullNodeShardImpl::got_neighbour_capabilities, id,
                                    td::Time::now() - start_time, R.move_as_ok());
          }
        });
    auto q = create_serialize_tl_object<ton_api::tonNode_getCapabilities>();
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, it->first, adnl_id_, overlay_id_,
                            "get_prepare_block", std::move(P), td::Timestamp::in(1.0), std::move(q));

    last_pinged_neighbour_ = it->first;
    it++;
    max_cnt--;
  }
}

FullNodeShardImpl::FullNodeShardImpl(ShardIdFull shard, PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id,
                                     FileHash zero_state_file_hash, td::actor::ActorId<keyring::Keyring> keyring,
                                     td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
                                     td::actor::ActorId<overlay::Overlays> overlays,
                                     td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                     td::actor::ActorId<adnl::AdnlExtClient> client)
    : shard_(shard)
    , local_id_(local_id)
    , adnl_id_(adnl_id)
    , zero_state_file_hash_(zero_state_file_hash)
    , keyring_(keyring)
    , adnl_(adnl)
    , rldp_(rldp)
    , overlays_(overlays)
    , validator_manager_(validator_manager)
    , client_(client) {
}

td::actor::ActorOwn<FullNodeShard> FullNodeShard::create(
    ShardIdFull shard, PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays,
    td::actor::ActorId<ValidatorManagerInterface> validator_manager, td::actor::ActorId<adnl::AdnlExtClient> client) {
  return td::actor::create_actor<FullNodeShardImpl>("tonnode", shard, local_id, adnl_id, zero_state_file_hash, keyring,
                                                    adnl, rldp, overlays, validator_manager, client);
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
