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
#include "td/utils/SharedSlice.h"
#include "full-node-master.hpp"
#include "full-node-shard-queries.hpp"

#include "ton/ton-shard.h"
#include "ton/ton-tl.hpp"

#include "adnl/utils.hpp"

#include "common/delay.h"

#include "auto/tl/lite_api.h"
#include "tl-utils/lite-utils.hpp"

namespace ton {

namespace validator {

namespace fullnode {

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getNextBlockDescription &query,
                                       td::Promise<td::BufferSlice> promise) {
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareBlock &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlock &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockFull &query,
                                       td::Promise<td::BufferSlice> promise) {
  td::actor::create_actor<BlockFullSender>("sender", ton::create_block_id(query.block_), false, validator_manager_,
                                           std::move(promise))
      .release();
}

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadNextBlockFull &query,
                                       td::Promise<td::BufferSlice> promise) {
  td::actor::create_actor<BlockFullSender>("sender", ton::create_block_id(query.prev_block_), true, validator_manager_,
                                           std::move(promise))
      .release();
}

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareBlockProof &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareKeyBlockProof &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockProof &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadBlockProofLink &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadKeyBlockProof &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadKeyBlockProofLink &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_prepareZeroState &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_preparePersistentState &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getNextKeyBlockIds &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadZeroState &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadPersistentState &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_downloadPersistentStateSlice &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getCapabilities &query,
                                       td::Promise<td::BufferSlice> promise) {
  promise.set_value(create_serialize_tl_object<ton_api::tonNode_capabilities>(proto_version(), proto_capabilities()));
}

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getArchiveInfo &query,
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

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_getArchiveSlice &query,
                                       td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_archive_slice, query.archive_id_,
                          query.offset_, query.max_size_, std::move(promise));
}

void FullNodeMasterImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::tonNode_slave_sendExtMessage &query,
                                       td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(
      validator_manager_, &ValidatorManagerInterface::run_ext_query,
      create_serialize_tl_object<lite_api::liteServer_query>(
          create_serialize_tl_object<lite_api::liteServer_sendMessage>(std::move(query.message_->data_))),
      [&](td::Result<td::BufferSlice>) {});
  promise.set_value(create_serialize_tl_object<ton_api::tonNode_success>());
}

void FullNodeMasterImpl::receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query,
                                       td::Promise<td::BufferSlice> promise) {
  auto BX = fetch_tl_prefix<ton_api::tonNode_query>(query, true);
  if (BX.is_error()) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "cannot parse tonnode query"));
    return;
  }
  auto B = fetch_tl_object<ton_api::Function>(std::move(query), true);
  if (B.is_error()) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "cannot parse tonnode query"));
    return;
  }
  ton_api::downcast_call(*B.move_as_ok().get(), [&](auto &obj) { this->process_query(src, obj, std::move(promise)); });
}

void FullNodeMasterImpl::start_up() {
  class Cb : public adnl::Adnl::Callback {
   public:
    Cb(td::actor::ActorId<FullNodeMasterImpl> id) : id_(std::move(id)) {
    }
    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeMasterImpl::receive_query, src, std::move(data), std::move(promise));
    }

   private:
    td::actor::ActorId<FullNodeMasterImpl> id_;
  };

  td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, adnl_id_,
                          adnl::Adnl::int_to_bytestring(ton_api::tonNode_query::ID),
                          std::make_unique<Cb>(actor_id(this)));

  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<adnl::AdnlExtServer>> R) {
        R.ensure();
        R.move_as_ok().release();
      });
  td::actor::send_closure(adnl_, &adnl::Adnl::create_ext_server, std::vector<adnl::AdnlNodeIdShort>{adnl_id_},
                          std::vector<td::uint16>{port_}, std::move(P));
}

FullNodeMasterImpl::FullNodeMasterImpl(adnl::AdnlNodeIdShort adnl_id, td::uint16 port, FileHash zero_state_file_hash,
                                       td::actor::ActorId<keyring::Keyring> keyring,
                                       td::actor::ActorId<adnl::Adnl> adnl,
                                       td::actor::ActorId<ValidatorManagerInterface> validator_manager)
    : adnl_id_(adnl_id)
    , port_(port)
    , zero_state_file_hash_(zero_state_file_hash)
    , keyring_(keyring)
    , adnl_(adnl)
    , validator_manager_(validator_manager) {
}

td::actor::ActorOwn<FullNodeMaster> FullNodeMaster::create(
    adnl::AdnlNodeIdShort adnl_id, td::uint16 port, FileHash zero_state_file_hash,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<ValidatorManagerInterface> validator_manager) {
  return td::actor::create_actor<FullNodeMasterImpl>("tonnode", adnl_id, port, zero_state_file_hash, keyring, adnl,
                                                     validator_manager);
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
