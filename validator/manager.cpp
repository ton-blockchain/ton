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
#include "manager.hpp"
#include "checksum.h"
#include "td/utils/buffer.h"
#include "validator-group.hpp"
#include "downloaders/wait-block-state.hpp"
#include "downloaders/wait-block-state-merge.hpp"
#include "downloaders/wait-block-data.hpp"
#include "fabric.h"
#include "manager.h"
#include "validate-broadcast.hpp"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"
#include "state-serializer.hpp"
#include "get-next-key-blocks.h"
#include "import-db-slice.hpp"

#include "auto/tl/lite_api.h"
#include "tl-utils/lite-utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "tl/tl_json.h"

#include "td/utils/Random.h"
#include "td/utils/port/path.h"
#include "td/utils/JsonBuilder.h"

#include "common/delay.h"
#include "td/utils/filesystem.h"

#include "validator/stats-merger.h"

#include <fstream>

namespace ton {

namespace validator {

void ValidatorManagerImpl::validate_block_is_next_proof(BlockIdExt prev_block_id, BlockIdExt next_block_id,
                                                        td::BufferSlice proof, td::Promise<td::Unit> promise) {
  if (!prev_block_id.is_masterchain() || !next_block_id.is_masterchain()) {
    VLOG(VALIDATOR_NOTICE) << "prev=" << prev_block_id << " next=" << next_block_id;
    promise.set_error(
        td::Status::Error(ErrorCode::protoviolation, "validate_block_is_next_proof() can only work for masterchain"));
    return;
  }
  if (prev_block_id.seqno() + 1 != next_block_id.seqno()) {
    VLOG(VALIDATOR_NOTICE) << "prev=" << prev_block_id << " next=" << next_block_id;
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "validate_block_is_next_proof(): bad seqno"));
    return;
  }
  CHECK(last_masterchain_state_.not_null());
  auto pp = create_proof(next_block_id, std::move(proof));
  if (pp.is_error()) {
    promise.set_error(pp.move_as_error_prefix("failed to create proof: "));
    return;
  }

  if (last_masterchain_seqno_ == prev_block_id.seqno()) {
    CHECK(last_masterchain_block_id_ == prev_block_id);

    auto P = td::PromiseCreator::lambda(
        [promise = std::move(promise), id = prev_block_id](td::Result<BlockHandle> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
            return;
          }
          auto handle = R.move_as_ok();
          CHECK(!handle->merge_before());
          if (handle->one_prev(true) != id) {
            promise.set_error(td::Status::Error(ErrorCode::protoviolation, "prev block mismatch"));
            return;
          }
          promise.set_value(td::Unit());
        });

    run_check_proof_query(next_block_id, pp.move_as_ok(), actor_id(this), td::Timestamp::in(2.0), std::move(P),
                          last_masterchain_state_, opts_->is_hardfork(next_block_id));
  } else {
    auto P =
        td::PromiseCreator::lambda([promise = std::move(promise), next_block_id](td::Result<BlockHandle> R) mutable {
          R.ensure();
          auto handle = R.move_as_ok();
          CHECK(handle->inited_next_left());
          if (handle->one_next(true) == next_block_id) {
            promise.set_value(td::Unit());
          } else {
            promise.set_error(td::Status::Error("next block id mismatch"));
          }
        });
    get_block_handle(prev_block_id, false, std::move(P));
  }
}

void ValidatorManagerImpl::validate_block_proof(BlockIdExt block_id, td::BufferSlice proof,
                                                td::Promise<td::Unit> promise) {
  auto pp = create_proof(block_id, std::move(proof));
  if (pp.is_error()) {
    promise.set_error(pp.move_as_error_prefix(PSTRING() << "failed to create proof for " << block_id << ": "));
    return;
  }

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(td::Unit());
    }
  });
  run_check_proof_query(block_id, pp.move_as_ok(), actor_id(this), td::Timestamp::in(2.0), std::move(P),
                        opts_->is_hardfork(block_id));
}

void ValidatorManagerImpl::validate_block_proof_link(BlockIdExt block_id, td::BufferSlice proof,
                                                     td::Promise<td::Unit> promise) {
  auto pp = create_proof_link(block_id, std::move(proof));
  if (pp.is_error()) {
    promise.set_error(pp.move_as_error_prefix(PSTRING() << "failed to create proof link for " << block_id << ": "));
    return;
  }

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(td::Unit());
    }
  });
  run_check_proof_link_query(block_id, pp.move_as_ok(), actor_id(this), td::Timestamp::in(2.0), std::move(P));
}

void ValidatorManagerImpl::validate_block_proof_rel(BlockIdExt block_id, BlockIdExt rel_block_id, td::BufferSlice proof,
                                                    td::Promise<td::Unit> promise) {
  auto pp = create_proof(block_id, std::move(proof));
  if (pp.is_error()) {
    promise.set_error(pp.move_as_error_prefix(PSTRING() << "failed to create proof for " << block_id << ": "));
    return;
  }

  auto Q = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      promise.set_value(td::Unit());
    }
  });

  if (rel_block_id.id.seqno == 0) {
    auto P = td::PromiseCreator::lambda(
        [block_id, SelfId = actor_id(this), proof = pp.move_as_ok(), promise = std::move(Q),
         skip_sig = opts_->is_hardfork(block_id)](td::Result<td::Ref<ShardState>> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            auto s = td::Ref<MasterchainState>{R.move_as_ok()};

            run_check_proof_query(block_id, std::move(proof), SelfId, td::Timestamp::in(2.0), std::move(promise),
                                  std::move(s), skip_sig);
          }
        });
    get_shard_state_from_db_short(rel_block_id, std::move(P));
  } else {
    auto P =
        td::PromiseCreator::lambda([block_id, SelfId = actor_id(this), proof = pp.move_as_ok(), promise = std::move(Q),
                                    skip_sig = opts_->is_hardfork(block_id)](td::Result<td::Ref<ProofLink>> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            run_check_proof_query(block_id, std::move(proof), SelfId, td::Timestamp::in(2.0), std::move(promise),
                                  R.move_as_ok(), skip_sig);
          }
        });
    get_block_proof_link_from_db_short(rel_block_id, std::move(P));
  }
}

void ValidatorManagerImpl::validate_block(ReceivedBlock block, td::Promise<BlockHandle> promise) {
  auto blkid = block.id;
  auto pp = create_block(std::move(block));
  if (pp.is_error()) {
    promise.set_error(pp.move_as_error_prefix(PSTRING() << "failed to create block for " << blkid << ": "));
    return;
  }
  CHECK(blkid.is_masterchain());

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise), id = blkid](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ValidatorManager::get_block_handle, id, true, std::move(promise));
        }
      });
  run_apply_block_query(block.id, pp.move_as_ok(), block.id, actor_id(this), td::Timestamp::in(10.0), std::move(P));
}

void ValidatorManagerImpl::prevalidate_block(BlockBroadcast broadcast, td::Promise<td::Unit> promise) {
  if (!started_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "node not started"));
    return;
  }
  if (!need_monitor(broadcast.block_id.shard_full())) {
    promise.set_error(td::Status::Error("not monitoring shard"));
    return;
  }
  promise = [SelfId = actor_id(this), promise = std::move(promise), block_id = broadcast.block_id,
             cc_seqno = broadcast.catchain_seqno](td::Result<td::Unit> R) mutable {
    if (R.is_ok()) {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::validated_block_broadcast, block_id, cc_seqno);
    }
    promise.set_result(std::move(R));
  };
  td::actor::create_actor<ValidateBroadcast>("broadcast", std::move(broadcast), last_masterchain_block_handle_,
                                             last_masterchain_state_, last_known_key_block_handle_, actor_id(this),
                                             td::Timestamp::in(2.0), std::move(promise))
      .release();
}

void ValidatorManagerImpl::validated_block_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno) {
}

void ValidatorManagerImpl::sync_complete(td::Promise<td::Unit> promise) {
  started_ = true;

  VLOG(VALIDATOR_WARNING) << "completed sync. Validating " << validator_groups_.size() << " groups";
  for (auto &v : validator_groups_) {
    if (!v.second.actor.empty()) {
      td::actor::send_closure(v.second.actor, &ValidatorGroup::create_session);
    }
  }
  for (auto &v : next_validator_groups_) {
    if (!v.second.actor.empty()) {
      td::actor::send_closure(v.second.actor, &ValidatorGroup::create_session);
    }
  }
}

void ValidatorManagerImpl::get_next_block(BlockIdExt block_id, td::Promise<BlockHandle> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        auto handle = R.move_as_ok();
        if (!handle->inited_next_left()) {
          promise.set_error(td::Status::Error(ErrorCode::notready, "next block not known"));
          return;
        }

        td::actor::send_closure(SelfId, &ValidatorManagerImpl::get_block_handle, handle->one_next(true), true,
                                std::move(promise));
      });

  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_next_key_blocks(BlockIdExt block_id, td::uint32 cnt,
                                               td::Promise<std::vector<BlockIdExt>> promise) {
  if (!last_masterchain_block_handle_ || !last_key_block_handle_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not inited"));
    return;
  }

  td::actor::create_actor<GetNextKeyBlocks>("nextkeyblocks", block_id, cnt, last_key_block_handle_,
                                            last_masterchain_state_, actor_id(this), td::Timestamp::in(2.0),
                                            std::move(promise))
      .release();
}

void ValidatorManagerImpl::get_block_data(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<BlockData>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto B = R.move_as_ok();
      promise.set_value(B->data());
    }
  });

  td::actor::send_closure(db_, &Db::get_block_data, std::move(handle), std::move(P));
}

void ValidatorManagerImpl::check_zero_state_exists(BlockIdExt block_id, td::Promise<bool> promise) {
  td::actor::send_closure(db_, &Db::check_zero_state_file_exists, block_id, std::move(promise));
}
void ValidatorManagerImpl::get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::get_zero_state_file, block_id, std::move(promise));
}

void ValidatorManagerImpl::get_persistent_state_size(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                     td::Promise<td::uint64> promise) {
  td::actor::send_closure(db_, &Db::get_persistent_state_file_size, block_id, masterchain_block_id,
                          std::move(promise));
}
void ValidatorManagerImpl::get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::get_persistent_state_file, block_id, masterchain_block_id, std::move(promise));
}

void ValidatorManagerImpl::get_persistent_state_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                      td::int64 offset, td::int64 max_length,
                                                      td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::get_persistent_state_file_slice, block_id, masterchain_block_id, offset, max_length,
                          std::move(promise));
}

void ValidatorManagerImpl::get_previous_persistent_state_files(
    BlockSeqno cur_mc_seqno, td::Promise<std::vector<std::pair<std::string, ShardIdFull>>> promise) {
  td::actor::send_closure(db_, &Db::get_previous_persistent_state_files, cur_mc_seqno, std::move(promise));
}

void ValidatorManagerImpl::get_block_proof(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto B = R.move_as_ok();
      promise.set_value(B->data());
    }
  });

  td::actor::send_closure(db_, &Db::get_block_proof, std::move(handle), std::move(P));
}

void ValidatorManagerImpl::get_block_proof_link(BlockHandle handle, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<ProofLink>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto B = R.move_as_ok();
      promise.set_value(B->data());
    }
  });

  td::actor::send_closure(db_, &Db::get_block_proof_link, std::move(handle), std::move(P));
}

void ValidatorManagerImpl::get_key_block_proof(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto B = R.move_as_ok();
      promise.set_value(B->data());
    }
  });

  td::actor::send_closure(db_, &Db::get_key_block_proof, block_id, std::move(P));
}

void ValidatorManagerImpl::get_key_block_proof_link(BlockIdExt block_id, td::Promise<td::BufferSlice> promise) {
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), block_id, db = db_.get()](td::Result<td::Ref<Proof>> R) mutable {
        if (R.is_error()) {
          auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<ProofLink>> R) mutable {
            if (R.is_error()) {
              promise.set_error(R.move_as_error());
            } else {
              auto B = R.move_as_ok();
              promise.set_value(B->data());
            }
          });

          td::actor::send_closure(db, &Db::get_key_block_proof_link, block_id, std::move(P));
        } else {
          auto B = R.move_as_ok()->export_as_proof_link().move_as_ok();
          promise.set_value(B->data());
        }
      });

  td::actor::send_closure(db_, &Db::get_key_block_proof, block_id, std::move(P));
}

void ValidatorManagerImpl::new_external_message(td::BufferSlice data, int priority) {
  if (!is_validator()) {
    return;
  }
  if (last_masterchain_state_.is_null()) {
    VLOG(VALIDATOR_NOTICE) << "dropping ext message: validator is not ready";
    return;
  }
  if (ext_msgs_[priority].ext_messages_.size() > (size_t)max_mempool_num()) {
    return;
  }
  auto R = create_ext_message(std::move(data), last_masterchain_state_->get_ext_msg_limits());
  if (R.is_error()) {
    VLOG(VALIDATOR_NOTICE) << "dropping bad ext message: " << R.move_as_error();
    return;
  }
  add_external_message(R.move_as_ok(), priority);
}

void ValidatorManagerImpl::add_external_message(td::Ref<ExtMessage> msg, int priority) {
  auto &msgs = ext_msgs_[priority];
  auto message = std::make_unique<MessageExt<ExtMessage>>(msg);
  auto id = message->ext_id();
  auto address = message->address();
  unsigned long per_address_limit = 256;
  auto it = msgs.ext_addr_messages_.find(address);
  if (it != msgs.ext_addr_messages_.end() && it->second.size() >= per_address_limit) {
    return;
  }
  auto it2 = ext_messages_hashes_.find(id.hash);
  if (it2 != ext_messages_hashes_.end()) {
    int old_priority = it2->second.first;
    if (old_priority >= priority) {
      return;
    }
    ext_msgs_[old_priority].erase(id);
  }
  msgs.ext_messages_.emplace(id, std::move(message));
  msgs.ext_addr_messages_[address].emplace(id.hash, id);
  ext_messages_hashes_[id.hash] = {priority, id};
}
void ValidatorManagerImpl::check_external_message(td::BufferSlice data, td::Promise<td::Ref<ExtMessage>> promise) {
  if (!started_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "node not synced"));
    return;
  }
  auto state = do_get_last_liteserver_state();
  if (state.is_null()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not ready"));
    return;
  }
  auto R = create_ext_message(std::move(data), state->get_ext_msg_limits());
  if (R.is_error()) {
    promise.set_error(R.move_as_error_prefix("failed to parse external message: "));
    return;
  }
  auto message = R.move_as_ok();
  WorkchainId wc = message->wc();
  StdSmcAddress addr = message->addr();
  if (checked_ext_msg_counter_.get_msg_count(wc, addr) >= max_ext_msg_per_addr()) {
    promise.set_error(
        td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex()));
    return;
  }

  promise = [self = this, wc, addr, promise = std::move(promise),
             SelfId = actor_id(this)](td::Result<td::Ref<ExtMessage>> R) mutable {
    td::actor::send_lambda(SelfId, [=, promise = std::move(promise), R = std::move(R)]() mutable {
      ++(R.is_ok() ? self->total_check_ext_messages_ok_ : self->total_check_ext_messages_error_);
      TRY_RESULT_PROMISE(promise, message, std::move(R));
      if (self->checked_ext_msg_counter_.inc_msg_count(wc, addr) > max_ext_msg_per_addr()) {
        promise.set_error(
            td::Status::Error(PSTRING() << "too many external messages to address " << wc << ":" << addr.to_hex()));
        return;
      }
      promise.set_result(std::move(message));
    });
  };
  ++ls_stats_check_ext_messages_;
  run_check_external_message(std::move(message), actor_id(this), std::move(promise));
}

void ValidatorManagerImpl::new_ihr_message(td::BufferSlice data) {
  if (!is_validator()) {
    return;
  }
  auto R = create_ihr_message(std::move(data));
  if (R.is_error()) {
    VLOG(VALIDATOR_NOTICE) << "dropping bad ihr message: " << R.move_as_error();
    return;
  }
  auto M = std::make_unique<MessageExt<IhrMessage>>(R.move_as_ok());
  auto id = M->ext_id();
  if (ihr_messages_hashes_.count(id.hash) == 0) {
    ihr_messages_.emplace(id, std::move(M));
    ihr_messages_hashes_.emplace(id.hash, id);
  }
}

void ValidatorManagerImpl::new_shard_block(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  if (!is_validator() && !cached_block_candidates_.count(block_id)) {
    return;
  }
  if (!last_masterchain_block_handle_) {
    VLOG(VALIDATOR_DEBUG) << "dropping top shard block broadcast: not inited";
    return;
  }
  if (!started_) {
    return;
  }
  auto it = shard_blocks_.find(ShardTopBlockDescriptionId{block_id.shard_full(), cc_seqno});
  if (it != shard_blocks_.end() && block_id.id.seqno <= it->second->block_id().id.seqno) {
    VLOG(VALIDATOR_DEBUG) << "dropping duplicate shard block broadcast";
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardTopBlockDescription>> R) {
    if (R.is_error()) {
      VLOG(VALIDATOR_NOTICE) << "dropping invalid new shard block description: " << R.move_as_error();
    } else {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::add_shard_block_description, R.move_as_ok());
    }
  });
  run_validate_shard_block_description(std::move(data), last_masterchain_block_handle_, last_masterchain_state_,
                                       actor_id(this), td::Timestamp::in(2.0), std::move(P));
}

void ValidatorManagerImpl::new_block_candidate(BlockIdExt block_id, td::BufferSlice data) {
  if (!last_masterchain_block_handle_) {
    VLOG(VALIDATOR_DEBUG) << "dropping top shard block broadcast: not inited";
    return;
  }
  if (!started_) {
    return;
  }
  if (!need_monitor(block_id.shard_full())) {
    VLOG(VALIDATOR_DEBUG) << "dropping block candidate broadcast: not monitoring shard";
    return;
  }
  add_cached_block_candidate(ReceivedBlock{block_id, std::move(data)});
}

void ValidatorManagerImpl::add_shard_block_description(td::Ref<ShardTopBlockDescription> desc) {
  if (!desc->may_be_valid(last_masterchain_block_handle_, last_masterchain_state_)) {
    return;
  }
  auto it = shard_blocks_.find(ShardTopBlockDescriptionId{desc->shard(), desc->catchain_seqno()});
  if (it != shard_blocks_.end() && desc->block_id().id.seqno <= it->second->block_id().id.seqno) {
    VLOG(VALIDATOR_DEBUG) << "dropping duplicate shard block broadcast";
    return;
  }
  shard_blocks_[ShardTopBlockDescriptionId{desc->block_id().shard_full(), desc->catchain_seqno()}] = desc;
  VLOG(VALIDATOR_DEBUG) << "new shard block descr for " << desc->block_id();
  if (need_monitor(desc->block_id().shard_full())) {
    auto P = td::PromiseCreator::lambda([](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        auto S = R.move_as_error();
        if (S.code() != ErrorCode::timeout && S.code() != ErrorCode::notready) {
          VLOG(VALIDATOR_NOTICE) << "failed to get shard state: " << S;
        } else {
          VLOG(VALIDATOR_DEBUG) << "failed to get shard state: " << S;
        }
      }
    });
    wait_block_state_short(desc->block_id(), 0, td::Timestamp::in(60.0), std::move(P));
  }
}

void ValidatorManagerImpl::add_cached_block_candidate(ReceivedBlock block) {
  BlockIdExt id = block.id;
  if (block.id.is_masterchain()) {
    return;
  }
  if (cached_block_candidates_.emplace(id, std::move(block)).second) {
    cached_block_candidates_lru_.push_back(id);
    {
      auto it = wait_block_data_.find(id);
      if (it != wait_block_data_.end()) {
        auto r_block = create_block(cached_block_candidates_[id].clone());
        if (r_block.is_ok()) {
          td::actor::send_closure(it->second.actor_, &WaitBlockData::loaded_block_data, r_block.move_as_ok());
        }
      }
    }
    {
      auto it = wait_state_.find(id);
      if (it != wait_state_.end()) {
        // Proof link is not ready at this point, but this will force WaitBlockState to redo send_get_proof_link_request
        td::actor::send_closure(it->second.actor_, &WaitBlockState::after_get_proof_link);
      }
    }
  }
  if (cached_block_candidates_lru_.size() > max_cached_candidates()) {
    CHECK(cached_block_candidates_.erase(cached_block_candidates_lru_.front()));
    cached_block_candidates_lru_.pop_front();
  }
}

void ValidatorManagerImpl::add_ext_server_id(adnl::AdnlNodeIdShort id) {
  class Cb : public adnl::Adnl::Callback {
   private:
    td::actor::ActorId<ValidatorManagerImpl> id_;

    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &ValidatorManagerImpl::run_ext_query, std::move(data), std::move(promise));
    }

   public:
    Cb(td::actor::ActorId<ValidatorManagerImpl> id) : id_(id) {
    }
  };

  td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, id,
                          adnl::Adnl::int_to_bytestring(lite_api::liteServer_query::ID),
                          std::make_unique<Cb>(actor_id(this)));

  if (lite_server_.empty()) {
    pending_ext_ids_.push_back(id);
  } else {
    td::actor::send_closure(lite_server_, &adnl::AdnlExtServer::add_local_id, id);
  }
}

void ValidatorManagerImpl::add_ext_server_port(td::uint16 port) {
  if (lite_server_.empty()) {
    pending_ext_ports_.push_back(port);
  } else {
    td::actor::send_closure(lite_server_, &adnl::AdnlExtServer::add_tcp_port, port);
  }
}

void ValidatorManagerImpl::created_ext_server(td::actor::ActorOwn<adnl::AdnlExtServer> server) {
  lite_server_ = std::move(server);
  for (auto &id : pending_ext_ids_) {
    td::actor::send_closure(lite_server_, &adnl::AdnlExtServer::add_local_id, id);
  }
  for (auto port : pending_ext_ports_) {
    td::actor::send_closure(lite_server_, &adnl::AdnlExtServer::add_tcp_port, port);
  }
  pending_ext_ids_.clear();
  pending_ext_ports_.clear();
}

void ValidatorManagerImpl::run_ext_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  if (!started_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "node not synced"));
    return;
  }
  auto F = fetch_tl_object<lite_api::liteServer_query>(data.clone(), true);
  if (F.is_ok()) {
    data = std::move(F.move_as_ok()->data_);
  } else {
    auto G = fetch_tl_prefix<lite_api::liteServer_queryPrefix>(data, true);
    if (G.is_error()) {
      promise.set_error(G.move_as_error());
      return;
    }
  }

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    td::BufferSlice data;
    if (R.is_error()) {
      auto S = R.move_as_error();
      data = create_serialize_tl_object<lite_api::liteServer_error>(S.code(), S.message().c_str());
    } else {
      data = R.move_as_ok();
    }
    promise.set_value(std::move(data));
  });

  auto E = fetch_tl_prefix<lite_api::liteServer_waitMasterchainSeqno>(data, true);
  if (E.is_error()) {
    run_liteserver_query(std::move(data), actor_id(this), lite_server_cache_.get(), std::move(P));
  } else {
    auto e = E.move_as_ok();
    if (static_cast<BlockSeqno>(e->seqno_) <= min_confirmed_masterchain_seqno_) {
      run_liteserver_query(std::move(data), actor_id(this), lite_server_cache_.get(), std::move(P));
    } else {
      auto t = e->timeout_ms_ < 10000 ? e->timeout_ms_ * 0.001 : 10.0;
      auto Q =
          td::PromiseCreator::lambda([data = std::move(data), SelfId = actor_id(this), cache = lite_server_cache_.get(),
                                      promise = std::move(P)](td::Result<td::Unit> R) mutable {
            if (R.is_error()) {
              promise.set_error(R.move_as_error());
              return;
            }
            run_liteserver_query(std::move(data), SelfId, cache, std::move(promise));
          });
      wait_shard_client_state(e->seqno_, td::Timestamp::in(t), std::move(Q));
    }
  }
}

void ValidatorManagerImpl::wait_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                            td::Promise<td::Ref<ShardState>> promise) {
  if (last_masterchain_state_.not_null() && !opts_->need_monitor(handle->id().shard_full(), last_masterchain_state_)) {
    return promise.set_error(
        td::Status::Error(PSTRING() << "not monitoring shard " << handle->id().shard_full().to_str()));
  }
  auto it0 = block_state_cache_.find(handle->id());
  if (it0 != block_state_cache_.end()) {
    it0->second.ttl_ = td::Timestamp::in(30.0);
    promise.set_result(it0->second.state_);
    return;
  }
  auto it = wait_state_.find(handle->id());
  if (it == wait_state_.end()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<ShardState>> R) {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::finished_wait_state, handle, std::move(R));
    });
    auto id = td::actor::create_actor<WaitBlockState>("waitstate", handle, priority, actor_id(this),
                                                      td::Timestamp::at(timeout.at() + 10.0), std::move(P),
                                                      get_block_persistent_state_to_download(handle->id()))
                  .release();
    wait_state_[handle->id()].actor_ = id;
    it = wait_state_.find(handle->id());
  }

  it->second.waiting_.emplace_back(timeout, priority, std::move(promise));
  auto X = it->second.get_timeout();
  td::actor::send_closure(it->second.actor_, &WaitBlockState::update_timeout, X.first, X.second);
}

void ValidatorManagerImpl::wait_block_state_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                                  td::Promise<td::Ref<ShardState>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), priority, timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_state, R.move_as_ok(), priority, timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_data(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                           td::Promise<td::Ref<BlockData>> promise) {
  auto it = wait_block_data_.find(handle->id());
  if (it == wait_block_data_.end()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<BlockData>> R) {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::finished_wait_data, handle, std::move(R));
    });
    auto id = td::actor::create_actor<WaitBlockData>("waitdata", handle, priority, actor_id(this),
                                                     td::Timestamp::at(timeout.at() + 10.0), false, std::move(P))
                  .release();
    wait_block_data_[handle->id()].actor_ = id;
    it = wait_block_data_.find(handle->id());
  }

  it->second.waiting_.emplace_back(timeout, priority, std::move(promise));
  auto X = it->second.get_timeout();
  td::actor::send_closure(it->second.actor_, &WaitBlockData::update_timeout, X.first, X.second);
}

void ValidatorManagerImpl::wait_block_data_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                                 td::Promise<td::Ref<BlockData>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), priority, timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_data, R.move_as_ok(), priority, timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_state_merge(BlockIdExt left_id, BlockIdExt right_id, td::uint32 priority,
                                                  td::Timestamp timeout, td::Promise<td::Ref<ShardState>> promise) {
  if (last_masterchain_state_.not_null() && !opts_->need_monitor(left_id.shard_full(), last_masterchain_state_)) {
    return promise.set_error(
        td::Status::Error(PSTRING() << "not monitoring shard " << left_id.shard_full().to_str()));
  }
  td::actor::create_actor<WaitBlockStateMerge>("merge", left_id, right_id, priority, actor_id(this), timeout,
                                               std::move(promise))
      .release();
}

void ValidatorManagerImpl::wait_prev_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                                 td::Promise<td::Ref<ShardState>> promise) {
  CHECK(handle);
  CHECK(!handle->is_zero());
  if (!handle->merge_before()) {
    auto shard = handle->id().shard_full();
    auto prev_shard = handle->one_prev(true).shard_full();
    if (shard == prev_shard) {
      wait_block_state_short(handle->one_prev(true), priority, timeout, std::move(promise));
    } else {
      CHECK(shard_parent(shard) == prev_shard);
      bool left = shard_child(prev_shard, true) == shard;
      auto P =
          td::PromiseCreator::lambda([promise = std::move(promise), left](td::Result<td::Ref<ShardState>> R) mutable {
            if (R.is_error()) {
              promise.set_error(R.move_as_error());
            } else {
              auto s = R.move_as_ok();
              auto r = s->split();
              if (r.is_error()) {
                promise.set_error(r.move_as_error());
              } else {
                auto v = r.move_as_ok();
                promise.set_value(left ? std::move(v.first) : std::move(v.second));
              }
            }
          });
      wait_block_state_short(handle->one_prev(true), priority, timeout, std::move(P));
    }
  } else {
    wait_block_state_merge(handle->one_prev(true), handle->one_prev(false), priority, timeout, std::move(promise));
  }
}

void ValidatorManagerImpl::wait_block_proof(BlockHandle handle, td::Timestamp timeout,
                                            td::Promise<td::Ref<Proof>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::wait_block_proof_short(BlockIdExt block_id, td::Timestamp timeout,
                                                  td::Promise<td::Ref<Proof>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_proof, R.move_as_ok(), timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_proof_link(BlockHandle handle, td::Timestamp timeout,
                                                 td::Promise<td::Ref<ProofLink>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof_link, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::wait_block_proof_link_short(BlockIdExt block_id, td::Timestamp timeout,
                                                       td::Promise<td::Ref<ProofLink>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_proof_link, R.move_as_ok(), timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_signatures(BlockHandle handle, td::Timestamp timeout,
                                                 td::Promise<td::Ref<BlockSignatureSet>> promise) {
  td::actor::send_closure(db_, &Db::get_block_signatures, handle, std::move(promise));
}

void ValidatorManagerImpl::wait_block_signatures_short(BlockIdExt block_id, td::Timestamp timeout,
                                                       td::Promise<td::Ref<BlockSignatureSet>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_signatures, R.move_as_ok(), timeout,
                                std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::wait_block_message_queue(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                                    td::Promise<td::Ref<MessageQueue>> promise) {
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<ShardState>> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
    } else {
      auto state = R.move_as_ok();
      promise.set_result(state->message_queue());
    }
  });

  wait_block_state(handle, priority, timeout, std::move(P));
}

void ValidatorManagerImpl::wait_block_message_queue_short(BlockIdExt block_id, td::uint32 priority,
                                                          td::Timestamp timeout,
                                                          td::Promise<td::Ref<MessageQueue>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), priority, timeout, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::wait_block_message_queue, R.move_as_ok(), priority,
                                timeout, std::move(promise));
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::get_external_messages(
    ShardIdFull shard, td::Promise<std::vector<std::pair<td::Ref<ExtMessage>, int>>> promise) {
  td::Timer t;
  size_t processed = 0, deleted = 0;
  std::vector<std::pair<td::Ref<ExtMessage>, int>> res;
  MessageId<ExtMessage> left{AccountIdPrefixFull{shard.workchain, shard.shard & (shard.shard - 1)}, Bits256::zero()};
  size_t total_msgs = 0;
  td::Random::Fast rnd;
  for (auto iter = ext_msgs_.rbegin(); iter != ext_msgs_.rend(); ++iter) {
    std::vector<std::pair<td::Ref<ExtMessage>, int>> cur_res;
    int priority = iter->first;
    auto &msgs = iter->second;
    auto it = msgs.ext_messages_.lower_bound(left);
    while (it != msgs.ext_messages_.end()) {
      auto s = it->first;
      if (!shard_contains(shard, s.dst)) {
        break;
      }
      ++processed;
      if (it->second->expired()) {
        msgs.ext_addr_messages_[it->second->address()].erase(it->first.hash);
        ext_messages_hashes_.erase(it->first.hash);
        it = msgs.ext_messages_.erase(it);
        ++deleted;
        continue;
      }
      if (it->second->is_active()) {
        cur_res.emplace_back(it->second->message(), priority);
      }
      it++;
    }
    td::random_shuffle(td::as_mutable_span(cur_res), rnd);
    res.insert(res.end(), cur_res.begin(), cur_res.end());
    total_msgs += msgs.ext_messages_.size();
  }
  LOG(WARNING) << "get_external_messages to shard " << shard.to_str() << " : time=" << t.elapsed()
               << " result_size=" << res.size() << " processed=" << processed << " expired=" << deleted
               << " total_size=" << total_msgs;
  promise.set_value(std::move(res));
}

void ValidatorManagerImpl::get_ihr_messages(ShardIdFull shard, td::Promise<std::vector<td::Ref<IhrMessage>>> promise) {
  std::vector<td::Ref<IhrMessage>> res;
  MessageId<IhrMessage> left{AccountIdPrefixFull{shard.workchain, shard.shard & (shard.shard - 1)}, Bits256::zero()};
  auto it = ihr_messages_.lower_bound(left);
  while (it != ihr_messages_.end()) {
    auto s = it->first;
    if (!shard_contains(shard, s.dst)) {
      break;
    }
    if (it->second->expired()) {
      ihr_messages_hashes_.erase(it->first.hash);
      it = ihr_messages_.erase(it);
      continue;
    }
    if (it->second->is_active()) {
      res.push_back(it->second->message());
    }
    it++;
  }
  promise.set_value(std::move(res));
}

void ValidatorManagerImpl::get_shard_blocks(BlockIdExt masterchain_block_id,
                                            td::Promise<std::vector<td::Ref<ShardTopBlockDescription>>> promise) {
  std::vector<td::Ref<ShardTopBlockDescription>> v;
  for (auto &b : shard_blocks_) {
    v.push_back(b.second);
  }
  promise.set_value(std::move(v));
}

void ValidatorManagerImpl::complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                                      std::vector<ExtMessage::Hash> to_delete) {
  for (auto &hash : to_delete) {
    auto it = ext_messages_hashes_.find(hash);
    if (it != ext_messages_hashes_.end()) {
      int priority = it->second.first;
      auto msg_id = it->second.second;
      ext_msgs_[priority].erase(msg_id);
      ext_messages_hashes_.erase(it);
    }
  }
  unsigned long soft_mempool_limit = 1024;
  for (auto &hash : to_delay) {
    auto it = ext_messages_hashes_.find(hash);
    if (it != ext_messages_hashes_.end()) {
      int priority = it->second.first;
      auto msg_id = it->second.second;
      auto &msgs = ext_msgs_[priority];
      auto it2 = msgs.ext_messages_.find(msg_id);
      if ((msgs.ext_messages_.size() < soft_mempool_limit) && it2->second->can_postpone()) {
        it2->second->postpone();
      } else {
        msgs.erase(msg_id);
        ext_messages_hashes_.erase(it);
      }
    }
  }
}

void ValidatorManagerImpl::complete_ihr_messages(std::vector<IhrMessage::Hash> to_delay,
                                                 std::vector<IhrMessage::Hash> to_delete) {
  for (auto &hash : to_delete) {
    auto it = ihr_messages_hashes_.find(hash);
    if (it != ihr_messages_hashes_.end()) {
      ihr_messages_.erase(it->second);
      ihr_messages_hashes_.erase(it);
    }
  }
  for (auto &hash : to_delay) {
    auto it = ihr_messages_hashes_.find(hash);
    if (it != ihr_messages_hashes_.end()) {
      auto it2 = ihr_messages_.find(it->second);
      CHECK(it2 != ihr_messages_.end());
      if (it2->second->can_postpone()) {
        it2->second->postpone();
      } else {
        ihr_messages_.erase(it2);
        ihr_messages_hashes_.erase(it);
      }
    }
  }
}

void ValidatorManagerImpl::get_block_data_from_db(ConstBlockHandle handle, td::Promise<td::Ref<BlockData>> promise) {
  td::actor::send_closure(db_, &Db::get_block_data, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::get_block_data_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) {
  auto P =
      td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(db, &Db::get_block_data, std::move(handle), std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_shard_state_from_db(ConstBlockHandle handle, td::Promise<td::Ref<ShardState>> promise) {
  td::actor::send_closure(db_, &Db::get_block_state, handle, std::move(promise));
}

void ValidatorManagerImpl::get_shard_state_from_db_short(BlockIdExt block_id,
                                                         td::Promise<td::Ref<ShardState>> promise) {
  auto P =
      td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(db, &Db::get_block_state, std::move(handle), std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_block_candidate_from_db(PublicKey source, BlockIdExt id,
                                                       FileHash collated_data_file_hash,
                                                       td::Promise<BlockCandidate> promise) {
  td::actor::send_closure(db_, &Db::get_block_candidate, source, id, collated_data_file_hash, std::move(promise));
}

void ValidatorManagerImpl::get_candidate_data_by_block_id_from_db(BlockIdExt id, td::Promise<td::BufferSlice> promise) {
  auto it = cached_block_candidates_.find(id);
  if (it != cached_block_candidates_.end()) {
    promise.set_result(it->second.data.clone());
    return;
  }
  td::actor::send_closure(db_, &Db::get_block_candidate_by_block_id, id,
                          promise.wrap([](BlockCandidate &&b) { return std::move(b.data); }));
}

void ValidatorManagerImpl::get_block_proof_from_db(ConstBlockHandle handle, td::Promise<td::Ref<Proof>> promise) {
  td::actor::send_closure(db_, &Db::get_block_proof, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::get_block_proof_from_db_short(BlockIdExt block_id, td::Promise<td::Ref<Proof>> promise) {
  auto P =
      td::PromiseCreator::lambda([db = db_.get(), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(db, &Db::get_block_proof, std::move(handle), std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_block_proof_link_from_db(ConstBlockHandle handle,
                                                        td::Promise<td::Ref<ProofLink>> promise) {
  if (handle->inited_proof_link()) {
    td::actor::send_closure(db_, &Db::get_block_proof_link, std::move(handle), std::move(promise));
  } else if (handle->inited_proof()) {
    auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Ref<Proof>> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        promise.set_result(R.move_as_ok()->export_as_proof_link());
      }
    });
    td::actor::send_closure(db_, &Db::get_block_proof, std::move(handle), std::move(P));
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not in db"));
  }
}

void ValidatorManagerImpl::get_block_proof_link_from_db_short(BlockIdExt block_id,
                                                              td::Promise<td::Ref<ProofLink>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          td::actor::send_closure(SelfId, &ValidatorManager::get_block_proof_link_from_db, std::move(handle),
                                  std::move(promise));
        }
      });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::get_block_by_lt_from_db(AccountIdPrefixFull account, LogicalTime lt,
                                                   td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(db_, &Db::get_block_by_lt, account, lt, std::move(promise));
}

void ValidatorManagerImpl::get_block_by_unix_time_from_db(AccountIdPrefixFull account, UnixTime ts,
                                                          td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(db_, &Db::get_block_by_unix_time, account, ts, std::move(promise));
}

void ValidatorManagerImpl::get_block_by_seqno_from_db(AccountIdPrefixFull account, BlockSeqno seqno,
                                                      td::Promise<ConstBlockHandle> promise) {
  td::actor::send_closure(db_, &Db::get_block_by_seqno, account, seqno, std::move(promise));
}

void ValidatorManagerImpl::finished_wait_state(BlockHandle handle, td::Result<td::Ref<ShardState>> R) {
  if (R.is_ok()) {
    block_state_cache_[handle->id()] = {R.ok(), td::Timestamp::in(30.0)};
  }
  auto it = wait_state_.find(handle->id());
  if (it != wait_state_.end()) {
    if (R.is_error()) {
      auto S = R.move_as_error();
      if (S.code() != ErrorCode::timeout) {
        for (auto &X : it->second.waiting_) {
          X.promise.set_error(S.clone());
        }
      } else if (it->second.waiting_.size() != 0) {
        auto X = it->second.get_timeout();
        auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<ShardState>> R) {
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::finished_wait_state, handle, std::move(R));
        });
        auto id =
            td::actor::create_actor<WaitBlockState>("waitstate", handle, X.second, actor_id(this), X.first,
                                                    std::move(P), get_block_persistent_state_to_download(handle->id()))
                .release();
        it->second.actor_ = id;
        return;
      }
    } else {
      auto r = R.move_as_ok();
      for (auto &X : it->second.waiting_) {
        X.promise.set_result(r);
      }
    }
    wait_state_.erase(it);
  }
}

void ValidatorManagerImpl::finished_wait_data(BlockHandle handle, td::Result<td::Ref<BlockData>> R) {
  auto it = wait_block_data_.find(handle->id());
  if (it != wait_block_data_.end()) {
    if (R.is_error()) {
      auto S = R.move_as_error();
      if (S.code() != ErrorCode::timeout) {
        for (auto &X : it->second.waiting_) {
          X.promise.set_error(S.clone());
        }
      } else {
        auto X = it->second.get_timeout();
        auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<BlockData>> R) {
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::finished_wait_data, handle, std::move(R));
        });
        auto id =
            td::actor::create_actor<WaitBlockData>("waitdata", handle, X.second, actor_id(this), X.first, false,
                                                   std::move(P))
            .release();
        it->second.actor_ = id;
        return;
      }
    } else {
      auto r = R.move_as_ok();
      for (auto &X : it->second.waiting_) {
        X.promise.set_result(r);
      }
    }
    wait_block_data_.erase(it);
  }
}

void ValidatorManagerImpl::set_block_state(BlockHandle handle, td::Ref<ShardState> state,
                                           td::Promise<td::Ref<ShardState>> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), handle, promise = std::move(promise)](td::Result<td::Ref<ShardState>> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(R.move_as_ok());
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::written_handle, std::move(handle), [](td::Unit) {});
        }
      });
  td::actor::send_closure(db_, &Db::store_block_state, handle, state, std::move(P));
}

void ValidatorManagerImpl::get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) {
  td::actor::send_closure(db_, &Db::get_cell_db_reader, std::move(promise));
}

void ValidatorManagerImpl::store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                       td::BufferSlice state, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_persistent_state_file, block_id, masterchain_block_id, std::move(state),
                          std::move(promise));
}

void ValidatorManagerImpl::store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                           std::function<td::Status(td::FileFd&)> write_data,
                                                           td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_persistent_state_file_gen, block_id, masterchain_block_id,
                          std::move(write_data), std::move(promise));
}

void ValidatorManagerImpl::store_zero_state_file(BlockIdExt block_id, td::BufferSlice state,
                                                 td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_zero_state_file, block_id, std::move(state), std::move(promise));
}

void ValidatorManagerImpl::set_block_data(BlockHandle handle, td::Ref<BlockData> data, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), data, handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(td::Unit());
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::written_handle, std::move(handle), [](td::Unit) {});
        }
      });
  td::actor::send_closure(db_, &Db::store_block_data, handle, std::move(data), std::move(P));
}

void ValidatorManagerImpl::set_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(td::Unit());
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::written_handle, std::move(handle), [](td::Unit) {});
        }
      });
  td::actor::send_closure(db_, &Db::store_block_proof, handle, std::move(proof), std::move(P));
}

void ValidatorManagerImpl::set_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof,
                                                td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          promise.set_value(td::Unit());
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::written_handle, std::move(handle), [](td::Unit) {});
        }
      });
  td::actor::send_closure(db_, &Db::store_block_proof_link, handle, std::move(proof), std::move(P));
}

/*void ValidatorManagerImpl::set_zero_state(ZeroStateIdExt id, td::Ref<ShardState> state, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_zero_state, id, std::move(state), std::move(promise));
}*/

void ValidatorManagerImpl::set_block_signatures(BlockHandle handle, td::Ref<BlockSignatureSet> signatures,
                                                td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::store_block_signatures, handle, std::move(signatures), std::move(promise));
}

void ValidatorManagerImpl::set_next_block(BlockIdExt block_id, BlockIdExt next, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), next, promise = std::move(promise)](td::Result<BlockHandle> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          auto handle = R.move_as_ok();
          handle->set_next(next);
          if (handle->need_flush()) {
            handle->flush(SelfId, handle, std::move(promise));
          } else {
            promise.set_value(td::Unit());
          }
        }
      });
  get_block_handle(block_id, true, std::move(P));
}

void ValidatorManagerImpl::set_block_candidate(BlockIdExt id, BlockCandidate candidate, CatchainSeqno cc_seqno,
                                               td::uint32 validator_set_hash, td::Promise<td::Unit> promise) {
  if (!candidates_buffer_.empty()) {
    td::actor::send_closure(candidates_buffer_, &CandidatesBuffer::add_new_candidate, id,
                            PublicKey{pubkeys::Ed25519{candidate.pubkey.as_bits256()}}, candidate.collated_file_hash);
  }
  if (!id.is_masterchain()) {
    add_cached_block_candidate(ReceivedBlock{id, candidate.data.clone()});
  }
  td::actor::send_closure(db_, &Db::store_block_candidate, std::move(candidate), std::move(promise));
}

void ValidatorManagerImpl::send_block_candidate_broadcast(BlockIdExt id, CatchainSeqno cc_seqno,
                                                          td::uint32 validator_set_hash, td::BufferSlice data) {
  callback_->send_block_candidate(id, cc_seqno, validator_set_hash, std::move(data));
}

void ValidatorManagerImpl::write_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), handle, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::written_handle, std::move(handle), std::move(promise));
        }
      });
  td::actor::send_closure(db_, &Db::store_block_handle, std::move(handle), std::move(P));
}

void ValidatorManagerImpl::written_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  bool received = handle->received();
  bool inited_state = handle->received_state();
  bool inited_proof = handle->id().is_masterchain() ? handle->inited_proof() : handle->inited_proof_link();

  if (handle->need_flush()) {
    handle->flush(actor_id(this), handle, std::move(promise));
    return;
  }

  if (received && inited_proof) {
    auto it = wait_block_data_.find(handle->id());
    if (it != wait_block_data_.end()) {
      td::actor::send_closure(it->second.actor_, &WaitBlockData::force_read_from_db);
    }
  }
  if (inited_state && inited_proof) {
    auto it = wait_state_.find(handle->id());
    if (it != wait_state_.end()) {
      td::actor::send_closure(it->second.actor_, &WaitBlockState::force_read_from_db);
    }
  } else {
    if (handle->inited_proof_link()) {
      auto it = wait_state_.find(handle->id());
      if (it != wait_state_.end()) {
        td::actor::send_closure(it->second.actor_, &WaitBlockState::after_get_proof_link);
      }
    }
    if (handle->id().is_masterchain() && handle->inited_proof()) {
      auto it = wait_state_.find(handle->id());
      if (it != wait_state_.end()) {
        td::actor::send_closure(it->second.actor_, &WaitBlockState::after_get_proof);
      }
    }
  }

  promise.set_value(td::Unit());
}

void ValidatorManagerImpl::new_block_cont(BlockHandle handle, td::Ref<ShardState> state,
                                          td::Promise<td::Unit> promise) {
  if (state->get_shard().is_masterchain() && handle->id().id.seqno > last_masterchain_seqno_) {
    if (handle->id().id.seqno == last_masterchain_seqno_ + 1) {
      last_masterchain_seqno_ = handle->id().id.seqno;
      last_masterchain_state_ = td::Ref<MasterchainState>{state};
      last_masterchain_block_id_ = handle->id();
      last_masterchain_block_handle_ = handle;
      last_masterchain_block_handle_->set_processed();

      new_masterchain_block();

      promise.set_value(td::Unit());

      while (true) {
        auto it = pending_masterchain_states_.find(last_masterchain_seqno_ + 1);
        if (it != pending_masterchain_states_.end()) {
          CHECK(it == pending_masterchain_states_.begin());
          last_masterchain_block_handle_ = std::move(std::get<0>(it->second));
          last_masterchain_state_ = std::move(std::get<1>(it->second));
          last_masterchain_block_id_ = last_masterchain_block_handle_->id();
          last_masterchain_seqno_ = last_masterchain_block_id_.id.seqno;
          CHECK(it->first == last_masterchain_seqno_);

          auto l_promise = std::move(std::get<2>(it->second));
          last_masterchain_block_handle_->set_processed();

          pending_masterchain_states_.erase(it);

          new_masterchain_block();

          for (auto &p : l_promise) {
            p.set_value(td::Unit());
          }
        } else {
          break;
        }
      }
    } else {
      auto it = pending_masterchain_states_.find(handle->id().id.seqno);
      if (it != pending_masterchain_states_.end()) {
        std::get<2>(it->second).emplace_back(std::move(promise));
      } else {
        std::vector<td::Promise<td::Unit>> v;
        v.emplace_back(std::move(promise));
        pending_masterchain_states_.emplace(
            handle->id().id.seqno,
            std::forward_as_tuple(handle, td::Ref<MasterchainState>{std::move(state)}, std::move(v)));
      }
    }
  } else {
    handle->set_processed();
    promise.set_value(td::Unit());
  }
}

void ValidatorManagerImpl::new_block(BlockHandle handle, td::Ref<ShardState> state, td::Promise<td::Unit> promise) {
  if (handle->is_applied()) {
    return new_block_cont(std::move(handle), std::move(state), std::move(promise));
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle, state = std::move(state),
                                         promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::new_block_cont, std::move(handle), std::move(state),
                                std::move(promise));
      }
    });
    td::actor::send_closure(db_, &Db::apply_block, handle, std::move(P));
  }
}

void ValidatorManagerImpl::get_block_handle(BlockIdExt id, bool force, td::Promise<BlockHandle> promise) {
  if (!id.is_valid()) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "bad block id"));
    return;
  }
  {
    // updates LRU position if found
    auto B = get_handle_from_lru(id);
    if (B) {
      CHECK(B->id() == id);
      promise.set_value(std::move(B));
      return;
    }
  }
  auto it = handles_.find(id);
  if (it != handles_.end()) {
    auto handle = it->second.lock();
    if (handle) {
      CHECK(handle->id() == id);
      promise.set_value(std::move(handle));
      return;
    } else {
      handles_.erase(it);
    }
  }

  auto it2 = wait_block_handle_.find(id);
  if (it2 != wait_block_handle_.end()) {
    it2->second.waiting_.emplace_back(std::move(promise));
    return;
  }

  wait_block_handle_.emplace(id, WaitBlockHandle{});
  wait_block_handle_[id].waiting_.emplace_back(std::move(promise));

  auto P = td::PromiseCreator::lambda([id, force = true, SelfId = actor_id(this)](td::Result<BlockHandle> R) mutable {
    BlockHandle handle;
    if (R.is_error()) {
      auto S = R.move_as_error();
      if (S.code() == ErrorCode::notready && force) {
        handle = create_empty_block_handle(id);
      } else {
        LOG(FATAL) << "db error: failed to get block " << id << ": " << S;
        return;
      }
    } else {
      handle = R.move_as_ok();
    }
    CHECK(handle);
    CHECK(handle->id() == id);
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::register_block_handle, std::move(handle));
  });

  td::actor::send_closure(db_, &Db::get_block_handle, id, std::move(P));
}

void ValidatorManagerImpl::register_block_handle(BlockHandle handle) {
  CHECK(handles_.find(handle->id()) == handles_.end());
  handles_.emplace(handle->id(), std::weak_ptr<BlockHandleInterface>(handle));
  add_handle_to_lru(handle);
  {
    auto it = wait_block_handle_.find(handle->id());
    CHECK(it != wait_block_handle_.end());
    for (auto &p : it->second.waiting_) {
      p.set_result(handle);
    }
    wait_block_handle_.erase(it);
  }
}

void ValidatorManagerImpl::get_top_masterchain_state(td::Promise<td::Ref<MasterchainState>> promise) {
  if (last_masterchain_state_.is_null()) {
    promise.set_error(td::Status::Error(ton::ErrorCode::notready, "not started"));
  } else {
    promise.set_result(last_masterchain_state_);
  }
}

td::Ref<MasterchainState> ValidatorManagerImpl::do_get_last_liteserver_state() {
  if (last_masterchain_state_.is_null()) {
    return {};
  }
  if (last_liteserver_state_.is_null()) {
    last_liteserver_state_ = last_masterchain_state_;
    return last_liteserver_state_;
  }
  if (last_liteserver_state_->get_seqno() == last_masterchain_state_->get_seqno()) {
    return last_liteserver_state_;
  }
  // If liteserver seqno (i.e. shard client) lags then use last masterchain state for liteserver
  // Allowed lag depends on the block rate
  double time_per_block = double(last_masterchain_state_->get_unix_time() - last_liteserver_state_->get_unix_time()) /
                          double(last_masterchain_state_->get_seqno() - last_liteserver_state_->get_seqno());
  if (td::Clocks::system() - double(last_liteserver_state_->get_unix_time()) > std::min(time_per_block * 8, 180.0)) {
    last_liteserver_state_ = last_masterchain_state_;
  }
  return last_liteserver_state_;
}

void ValidatorManagerImpl::get_top_masterchain_block(td::Promise<BlockIdExt> promise) {
  if (!last_masterchain_block_id_.is_valid()) {
    promise.set_error(td::Status::Error(ton::ErrorCode::notready, "not started"));
  } else {
    promise.set_result(last_masterchain_block_id_);
  }
}

void ValidatorManagerImpl::get_top_masterchain_state_block(
    td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) {
  if (last_masterchain_state_.is_null()) {
    promise.set_error(td::Status::Error(ton::ErrorCode::notready, "not started"));
  } else {
    promise.set_result(
        std::pair<td::Ref<MasterchainState>, BlockIdExt>{last_masterchain_state_, last_masterchain_block_id_});
  }
}

void ValidatorManagerImpl::get_last_liteserver_state_block(
    td::Promise<std::pair<td::Ref<MasterchainState>, BlockIdExt>> promise) {
  auto state = do_get_last_liteserver_state();
  if (state.is_null()) {
    promise.set_error(td::Status::Error(ton::ErrorCode::notready, "not started"));
  } else {
    promise.set_result(std::pair<td::Ref<MasterchainState>, BlockIdExt>{state, state->get_block_id()});
  }
}

void ValidatorManagerImpl::send_get_block_request(BlockIdExt id, td::uint32 priority,
                                                  td::Promise<ReceivedBlock> promise) {
  {
    auto it = cached_block_candidates_.find(id);
    if (it != cached_block_candidates_.end()) {
      LOG(DEBUG) << "send_get_block_request: got result from candidates cache for " << id.to_str();
      return promise.set_value(it->second.clone());
    }
  }
  callback_->download_block(id, priority, td::Timestamp::in(10.0), std::move(promise));
}

void ValidatorManagerImpl::send_get_zero_state_request(BlockIdExt id, td::uint32 priority,
                                                       td::Promise<td::BufferSlice> promise) {
  callback_->download_zero_state(id, priority, td::Timestamp::in(10.0), std::move(promise));
}

void ValidatorManagerImpl::send_get_persistent_state_request(BlockIdExt id, BlockIdExt masterchain_block_id,
                                                             td::uint32 priority,
                                                             td::Promise<td::BufferSlice> promise) {
  callback_->download_persistent_state(id, masterchain_block_id, priority, td::Timestamp::in(3600 * 3),
                                       std::move(promise));
}

void ValidatorManagerImpl::send_get_block_proof_request(BlockIdExt block_id, td::uint32 priority,
                                                        td::Promise<td::BufferSlice> promise) {
  callback_->download_block_proof(block_id, priority, td::Timestamp::in(10.0), std::move(promise));
}

void ValidatorManagerImpl::send_get_block_proof_link_request(BlockIdExt block_id, td::uint32 priority,
                                                             td::Promise<td::BufferSlice> promise) {
  if (!block_id.is_masterchain()) {
    auto it = cached_block_candidates_.find(block_id);
    if (it != cached_block_candidates_.end()) {
      // Proof link can be created from the cached block candidate
      LOG(DEBUG) << "send_get_block_proof_link_request: creating proof link from cached caniddate for "
                 << block_id.to_str();
      TRY_RESULT_PROMISE_PREFIX(promise, block_root, vm::std_boc_deserialize(it->second.data),
                                "failed to create proof link: ");
      TRY_RESULT_PROMISE_PREFIX(promise, proof_link, WaitBlockData::generate_proof_link(it->second.id, block_root),
                                "failed to create proof link: ");
      promise.set_result(std::move(proof_link));
      return;
    }
  }
  callback_->download_block_proof_link(block_id, priority, td::Timestamp::in(10.0), std::move(promise));
}

void ValidatorManagerImpl::send_get_next_key_blocks_request(BlockIdExt block_id, td::uint32 priority,
                                                            td::Promise<std::vector<BlockIdExt>> promise) {
  callback_->get_next_key_blocks(block_id, td::Timestamp::in(10.0), std::move(promise));
}

void ValidatorManagerImpl::send_external_message(td::Ref<ExtMessage> message) {
  callback_->send_ext_message(message->shard(), message->serialize());
  add_external_message(std::move(message), 0);
}

void ValidatorManagerImpl::send_ihr_message(td::Ref<IhrMessage> message) {
  callback_->send_ihr_message(message->shard(), message->serialize());
}

void ValidatorManagerImpl::send_top_shard_block_description(td::Ref<ShardTopBlockDescription> desc) {
  if (!resend_shard_blocks_at_) {
    resend_shard_blocks_at_ = td::Timestamp::in(td::Random::fast(0, 100) * 0.01 + 2.0);
    alarm_timestamp().relax(resend_shard_blocks_at_);
  }
  auto it = out_shard_blocks_.find(ShardTopBlockDescriptionId{desc->block_id().shard_full(), desc->catchain_seqno()});
  if (it != out_shard_blocks_.end() && desc->block_id().id.seqno <= it->second->block_id().id.seqno) {
    VLOG(VALIDATOR_DEBUG) << "dropping duplicate top block description";
  } else {
    out_shard_blocks_[ShardTopBlockDescriptionId{desc->block_id().shard_full(), desc->catchain_seqno()}] = desc;
    callback_->send_shard_block_info(desc->block_id(), desc->catchain_seqno(), desc->serialize());
    add_shard_block_description(desc);
  }
}

void ValidatorManagerImpl::send_block_broadcast(BlockBroadcast broadcast, int mode) {
  callback_->send_broadcast(std::move(broadcast), mode);
}

void ValidatorManagerImpl::send_validator_telemetry(PublicKeyHash key,
                                                    tl_object_ptr<ton_api::validator_telemetry> telemetry) {
  callback_->send_validator_telemetry(key, std::move(telemetry));
}

void ValidatorManagerImpl::send_get_out_msg_queue_proof_request(
    ShardIdFull dst_shard, std::vector<BlockIdExt> blocks, block::ImportedMsgQueueLimits limits,
    td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) {
  callback_->download_out_msg_queue_proof(dst_shard, std::move(blocks), limits, td::Timestamp::in(10.0),
                                          std::move(promise));
}

void ValidatorManagerImpl::send_download_archive_request(BlockSeqno mc_seqno, ShardIdFull shard_prefix,
                                                         std::string tmp_dir, td::Timestamp timeout,
                                                         td::Promise<std::string> promise) {
  callback_->download_archive(mc_seqno, shard_prefix, std::move(tmp_dir), timeout, std::move(promise));
}

void ValidatorManagerImpl::start_up() {
  db_ = create_db_actor(actor_id(this), db_root_, opts_);
  actor_stats_ = td::actor::create_actor<td::actor::ActorStats>("actor_stats");
  lite_server_cache_ = create_liteserver_cache_actor(actor_id(this), db_root_);
  token_manager_ = td::actor::create_actor<TokenManager>("tokenmanager");
  td::mkdir(db_root_ + "/tmp/").ensure();
  td::mkdir(db_root_ + "/catchains/").ensure();

  auto Q =
      td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<adnl::AdnlExtServer>> R) {
        R.ensure();
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::created_ext_server, R.move_as_ok());
      });
  td::actor::send_closure(adnl_, &adnl::Adnl::create_ext_server, std::vector<adnl::AdnlNodeIdShort>{},
                          std::vector<td::uint16>{}, std::move(Q));

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ValidatorManagerInitResult> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::started, R.move_as_ok());
  });

  auto to_import_dir = db_root_ + "/import";
  auto S = td::WalkPath::run(to_import_dir, [&](td::CSlice cfname, td::WalkPath::Type t) -> void {
    auto fname = td::Slice(cfname);
    if (t == td::WalkPath::Type::NotDir) {
      auto d = fname.rfind(TD_DIR_SLASH);
      if (d != td::Slice::npos) {
        fname = fname.substr(d + 1);
      }
      if (fname.size() <= 13) {
        return;
      }
      if (fname.substr(fname.size() - 5) != ".pack") {
        return;
      }
      if (fname.substr(0, 8) != "archive.") {
        return;
      }
      fname = fname.substr(8);

      while (fname.size() > 1 && fname[0] == '0') {
        fname.remove_prefix(1);
      }
      auto i = fname.find('.');
      if (i == td::Slice::npos) {
        return;
      }
      fname = fname.substr(0, i);
      auto v = td::to_integer_safe<BlockSeqno>(fname);
      if (v.is_error()) {
        return;
      }
      auto seqno = v.move_as_ok();
      LOG(INFO) << "found archive slice '" << cfname << "' for seqno " << seqno;
      to_import_[seqno].push_back(cfname.str());
    }
  });
  if (S.is_error()) {
    LOG(INFO) << "failed to load blocks from import dir: " << S;
  }

  validator_manager_init(opts_, actor_id(this), db_.get(), std::move(P));

  check_waiters_at_ = td::Timestamp::in(1.0);
  alarm_timestamp().relax(check_waiters_at_);
}

void ValidatorManagerImpl::init_last_masterchain_state(td::Ref<MasterchainState> state) {
  if (last_masterchain_state_.not_null()) {
    return;
  }
  last_masterchain_state_ = std::move(state);
  update_shard_overlays();
}

void ValidatorManagerImpl::started(ValidatorManagerInitResult R) {
  CHECK(R.handle);
  CHECK(R.state.not_null());
  last_masterchain_block_handle_ = std::move(R.handle);
  last_masterchain_block_id_ = last_masterchain_block_handle_->id();
  last_masterchain_seqno_ = last_masterchain_block_id_.id.seqno;
  last_masterchain_state_ = std::move(R.state);

  last_key_block_handle_ = std::move(R.last_key_block_handle_);
  last_known_key_block_handle_ = last_key_block_handle_;

  CHECK(last_masterchain_block_handle_->is_applied());
  if (last_known_key_block_handle_->inited_is_key_block()) {
    callback_->new_key_block(last_key_block_handle_);
  }

  gc_masterchain_handle_ = std::move(R.gc_handle);
  gc_masterchain_state_ = std::move(R.gc_state);

  shard_client_ = std::move(R.clients);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<ValidatorSessionId>> R) {
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::notready) {
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::read_gc_list, std::vector<ValidatorSessionId>{});
      } else {
        LOG(FATAL) << "db error: " << R.move_as_error();
      }
    } else {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::read_gc_list, R.move_as_ok());
    }
  });
  td::actor::send_closure(db_, &Db::get_destroyed_validator_sessions, std::move(P));

  if (opts_->nonfinal_ls_queries_enabled()) {
    candidates_buffer_ = td::actor::create_actor<CandidatesBuffer>("candidates-buffer", actor_id(this));
  }
  init_validator_telemetry();

  auto Q = td::PromiseCreator::lambda(
      [SelfId = actor_id(this)](td::Result<std::vector<td::Ref<PersistentStateDescription>>> R) {
        if (R.is_error()) {
          LOG(FATAL) << "db error: " << R.move_as_error();
        } else {
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::got_persistent_state_descriptions, R.move_as_ok());
        }
      });
  td::actor::send_closure(db_, &Db::get_persistent_state_descriptions, std::move(Q));
  update_shard_overlays();
}

void ValidatorManagerImpl::read_gc_list(std::vector<ValidatorSessionId> list) {
  for (auto &v : list) {
    check_gc_list_.insert(v);
  }

  new_masterchain_block();

  serializer_ =
      td::actor::create_actor<AsyncStateSerializer>("serializer", last_key_block_handle_->id(), opts_, actor_id(this));
  td::actor::send_closure(serializer_, &AsyncStateSerializer::update_last_known_key_block_ts,
                          last_key_block_handle_->unix_time());

  if (last_masterchain_block_handle_->inited_next_left()) {
    auto b = last_masterchain_block_handle_->one_next(true);
    if (opts_->is_hardfork(b) && !out_of_sync()) {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), b](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          LOG(INFO) << "NO HARDFORK BLOCK IN STATIC FILES";
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::applied_hardfork);
          return;
        }

        auto dataR = create_block(b, R.move_as_ok());
        dataR.ensure();

        auto P = td::PromiseCreator::lambda([SelfId](td::Result<td::Unit> R) {
          R.ensure();
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::applied_hardfork);
        });
        run_hardfork_accept_block_query(b, dataR.move_as_ok(), SelfId, std::move(P));
      });
      td::actor::send_closure(db_, &Db::try_get_static_file, b.file_hash, std::move(P));
      return;
    }
  }

  if (!out_of_sync()) {
    completed_prestart_sync();
  } else {
    prestart_sync();
  }
}

void ValidatorManagerImpl::applied_hardfork() {
  if (!out_of_sync()) {
    completed_prestart_sync();
  } else {
    prestart_sync();
  }
}

bool ValidatorManagerImpl::out_of_sync() {
  auto seqno = std::min(last_masterchain_seqno_, shard_client_handle_->id().seqno());
  if (seqno < opts_->sync_upto()) {
    return true;
  }
  if (shard_client_handle_->id().seqno() + 16 < last_masterchain_seqno_) {
    return true;
  }
  if (last_masterchain_block_handle_->unix_time() + 600 > td::Clocks::system()) {
    return false;
  }

  if (last_masterchain_seqno_ < last_known_key_block_handle_->id().seqno()) {
    return true;
  }

  bool masterchain_validator = false;
  if (!validator_groups_.size()) {
    auto val_set = last_masterchain_state_->get_validator_set(ShardIdFull{masterchainId});
    if (!get_validator(ShardIdFull{masterchainId}, val_set).is_zero()) {
      masterchain_validator = true;
    }
  }

  if ((masterchain_validator || validator_groups_.size() > 0) &&
      last_known_key_block_handle_->id().seqno() <= last_masterchain_seqno_) {
    return false;
  }
  LOG(INFO) << "groups=" << validator_groups_.size() << " seqno=" << last_known_key_block_handle_->id().seqno()
            << " our_seqno=" << last_masterchain_seqno_;

  return true;
}

void ValidatorManagerImpl::prestart_sync() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::download_next_archive);
  });
  td::actor::send_closure(db_, &Db::set_async_mode, false, std::move(P));
}

void ValidatorManagerImpl::download_next_archive() {
  if (!out_of_sync()) {
    finish_prestart_sync();
    return;
  }

  auto seqno = std::min(last_masterchain_seqno_, shard_client_handle_->id().seqno());
  std::vector<std::string> to_import_files;
  auto it = to_import_.upper_bound(seqno + 1);
  if (it != to_import_.begin()) {
    --it;
    to_import_files = std::move(it->second);
    it->second.clear();
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::pair<BlockSeqno, BlockSeqno>> R) {
    if (R.is_error()) {
      LOG(INFO) << "failed to download and import archive slice: " << R.error();
      delay_action([SelfId]() { td::actor::send_closure(SelfId, &ValidatorManagerImpl::download_next_archive); },
                   td::Timestamp::in(2.0));
    } else {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::checked_archive_slice, R.ok().first, R.ok().second);
    }
  });
  td::actor::create_actor<ArchiveImporter>("archiveimport", db_root_, last_masterchain_state_, seqno, opts_,
                                           actor_id(this), std::move(to_import_files), std::move(P))
      .release();
}

void ValidatorManagerImpl::checked_archive_slice(BlockSeqno new_last_mc_seqno, BlockSeqno new_shard_client_seqno) {
  LOG(INFO) << "checked downloaded archive slice: mc_top_seqno=" << new_last_mc_seqno
            << " shard_top_seqno_=" << new_shard_client_seqno;
  CHECK(new_last_mc_seqno <= last_masterchain_seqno_);
  CHECK(new_shard_client_seqno <= last_masterchain_seqno_);

  BlockIdExt shard_client_block_id;
  CHECK(last_masterchain_state_->get_old_mc_block_id(new_shard_client_seqno, shard_client_block_id));

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), db = db_.get(), client = shard_client_.get()](td::Result<BlockHandle> R) {
        R.ensure();
        auto handle = R.move_as_ok();
        auto P = td::PromiseCreator::lambda([SelfId, client, handle](td::Result<td::Ref<ShardState>> R) mutable {
          auto P = td::PromiseCreator::lambda([SelfId](td::Result<td::Unit> R) {
            R.ensure();
            td::actor::send_closure(SelfId, &ValidatorManagerImpl::download_next_archive);
          });
          td::actor::send_closure(client, &ShardClient::force_update_shard_client_ex, std::move(handle),
                                  td::Ref<MasterchainState>{R.move_as_ok()}, std::move(P));
        });
        td::actor::send_closure(db, &Db::get_block_state, std::move(handle), std::move(P));
      });
  get_block_handle(shard_client_block_id, true, std::move(P));
}

void ValidatorManagerImpl::finish_prestart_sync() {
  to_import_.clear();

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::completed_prestart_sync);
  });
  td::actor::send_closure(db_, &Db::set_async_mode, false, std::move(P));
}

void ValidatorManagerImpl::completed_prestart_sync() {
  td::actor::send_closure(shard_client_, &ShardClient::start);

  send_peek_key_block_request();

  LOG(WARNING) << "initial read complete: " << last_masterchain_block_handle_->id() << " "
               << last_masterchain_block_id_;
  callback_->initial_read_complete(last_masterchain_block_handle_);
}

void ValidatorManagerImpl::new_masterchain_block() {
  if (last_masterchain_seqno_ > 0 && last_masterchain_block_handle_->is_key_block()) {
    last_key_block_handle_ = last_masterchain_block_handle_;
    if (last_key_block_handle_->id().seqno() > last_known_key_block_handle_->id().seqno()) {
      last_known_key_block_handle_ = last_key_block_handle_;
      callback_->new_key_block(last_key_block_handle_);
    }
    if (!serializer_.empty()) {
      td::actor::send_closure(serializer_, &AsyncStateSerializer::update_last_known_key_block_ts,
                              last_key_block_handle_->unix_time());
    }
    init_validator_telemetry();
  }

  update_shard_overlays();
  update_shards();
  update_shard_blocks();

  if (!shard_client_.empty()) {
    td::actor::send_closure(shard_client_, &ShardClient::new_masterchain_block_notification,
                            last_masterchain_block_handle_, last_masterchain_state_);
  }

  if (last_masterchain_seqno_ % 1024 == 0) {
    LOG(WARNING) << "applied masterchain block " << last_masterchain_block_id_;
  }
}

void ValidatorManagerImpl::update_shard_overlays() {
  CHECK(last_masterchain_state_.not_null());
  std::set<ShardIdFull> shards_to_monitor;
  shards_to_monitor.insert(ShardIdFull{masterchainId});
  std::set<WorkchainId> workchains;
  for (const auto& shard : last_masterchain_state_->get_shards()) {
    workchains.insert(shard->shard().workchain);
    if (opts_->need_monitor(shard->shard(),last_masterchain_state_)) {
      shards_to_monitor.insert(shard->shard());
    }
  }
  for (const auto &[wc, desc] : last_masterchain_state_->get_workchain_list()) {
    if (!workchains.count(wc) && desc->active &&
        opts_->need_monitor(ShardIdFull{wc, shardIdAll}, last_masterchain_state_)) {
      shards_to_monitor.insert(ShardIdFull{wc, shardIdAll});
    }
  }
  callback_->on_new_masterchain_block(last_masterchain_state_, std::move(shards_to_monitor));
}

void ValidatorManagerImpl::update_shards() {
  if ((last_masterchain_state_->rotated_all_shards() || last_masterchain_seqno_ == 0) &&
      opts_->get_last_fork_masterchain_seqno() <= last_masterchain_seqno_) {
    allow_validate_ = true;
  }
  auto exp_vec = last_masterchain_state_->get_shards();
  auto config = last_masterchain_state_->get_consensus_config();
  validatorsession::ValidatorSessionOptions opts{config};
  td::uint32 threshold = 9407194;
  bool force_group_id_upgrade = last_masterchain_seqno_ == threshold;
  auto legacy_opts_hash = opts.get_hash();
  if (last_masterchain_seqno_ >= threshold) { //TODO move to get_consensus_config()
    opts.proto_version = std::max<td::uint32>(opts.proto_version, 1);
  }
  auto opts_hash = opts.get_hash();

  std::map<ShardIdFull, std::vector<BlockIdExt>> new_shards;
  std::set<ShardIdFull> future_shards;

  auto cur_time = static_cast<UnixTime>(td::Clocks::system());

  for (auto &v : exp_vec) {
    auto shard = v->shard();
    if (v->before_split()) {
      CHECK(!v->before_merge());
      ShardIdFull l_shard{shard.workchain, shard_child(shard.shard, true)};
      ShardIdFull r_shard{shard.workchain, shard_child(shard.shard, false)};
      new_shards.emplace(l_shard, std::vector<BlockIdExt>{v->top_block_id()});
      new_shards.emplace(r_shard, std::vector<BlockIdExt>{v->top_block_id()});
    } else if (v->before_merge()) {
      ShardIdFull p_shard{shard.workchain, shard_parent(shard.shard)};
      auto it = new_shards.find(p_shard);
      if (it == new_shards.end()) {
        new_shards.emplace(p_shard, std::vector<BlockIdExt>(2));
      }

      bool left = shard_child(p_shard.shard, true) == shard.shard;
      new_shards[p_shard][left ? 0 : 1] = v->top_block_id();
    } else {
      new_shards.emplace(shard, std::vector<BlockIdExt>{v->top_block_id()});
    }
    switch (v->fsm_state()) {
      case McShardHash::FsmState::fsm_none: {
        future_shards.insert(shard);
        break;
      }
      case McShardHash::FsmState::fsm_split: {
        if (v->fsm_utime() < cur_time + 60) {
          ShardIdFull l_shard{shard.workchain, shard_child(shard.shard, true)};
          ShardIdFull r_shard{shard.workchain, shard_child(shard.shard, false)};
          future_shards.insert(l_shard);
          future_shards.insert(r_shard);
        } else {
          future_shards.insert(shard);
        }
        break;
      }
      case McShardHash::FsmState::fsm_merge: {
        if (v->fsm_utime() < cur_time + 60) {
          ShardIdFull p_shard{shard.workchain, shard_parent(shard.shard)};
          future_shards.insert(p_shard);
        } else {
          future_shards.insert(shard);
        }
        break;
      }
      default:
        LOG(FATAL) << "state=" << static_cast<td::uint32>(v->fsm_state());
    }
  }

  new_shards.emplace(ShardIdFull{masterchainId, shardIdAll}, std::vector<BlockIdExt>{last_masterchain_block_id_});
  future_shards.insert(ShardIdFull{masterchainId, shardIdAll});

  VLOG(VALIDATOR_DEBUG) << "total shards=" << new_shards.size() << " config shards=" << exp_vec.size();

  std::map<ValidatorSessionId, ValidatorGroupEntry> new_validator_groups_;
  std::map<ValidatorSessionId, ValidatorGroupEntry> new_next_validator_groups_;

  bool force_recover = false;
  {
    auto val_set = last_masterchain_state_->get_validator_set(ShardIdFull{masterchainId});
    auto r = opts_->check_unsafe_catchain_rotate(last_masterchain_seqno_, val_set->get_catchain_seqno());
    force_recover = r > 0;
  }

  BlockSeqno key_seqno = last_key_block_handle_->id().seqno();

  if (force_group_id_upgrade) {
    for (auto &desc : new_shards) {
      auto shard = desc.first;
      auto prev = desc.second;
      for (auto &p : prev) {
        CHECK(p.is_valid());
      }
      auto val_set = last_masterchain_state_->get_validator_set(shard);
      auto validator_id = get_validator(shard, val_set);

      if (!validator_id.is_zero()) {
        auto legacy_val_group_id = get_validator_set_id(shard, val_set, legacy_opts_hash, key_seqno, opts);
        auto val_group_id = get_validator_set_id(shard, val_set, opts_hash, key_seqno, opts);


        auto it = validator_groups_.find(legacy_val_group_id);
        if (it != validator_groups_.end()) {
          new_validator_groups_.emplace(val_group_id, std::move(it->second));
        } else {
          auto it2 = next_validator_groups_.find(legacy_val_group_id);
          if (it2 != next_validator_groups_.end()) {
            if (!it2->second.actor.empty()) {
              td::actor::send_closure(it2->second.actor, &ValidatorGroup::start, prev, last_masterchain_block_id_);
            }
            new_validator_groups_.emplace(val_group_id, std::move(it2->second));
          } else {
            auto G = create_validator_group(val_group_id, shard, val_set, key_seqno, opts, started_);
            if (!G.empty()) {
              td::actor::send_closure(G, &ValidatorGroup::start, prev, last_masterchain_block_id_);
            }
            new_validator_groups_.emplace(val_group_id, ValidatorGroupEntry{std::move(G), shard});
          }
        }
      }
    }
  }

  active_validator_groups_master_ = active_validator_groups_shard_ = 0;
  if (allow_validate_) {
    for (auto &desc : new_shards) {
      auto shard = desc.first;
      if (force_recover && !desc.first.is_masterchain()) {
        continue;
      }
      auto prev = desc.second;
      for (auto &p : prev) {
        CHECK(p.is_valid());
      }
      auto val_set = last_masterchain_state_->get_validator_set(shard);
      auto x = val_set->export_vector();

      auto validator_id = get_validator(shard, val_set);

      if (!validator_id.is_zero()) {
        ++(shard.is_masterchain() ? active_validator_groups_master_ : active_validator_groups_shard_);
        auto val_group_id = get_validator_set_id(shard, val_set, opts_hash, key_seqno, opts);

        if (force_recover) {
          auto r = opts_->check_unsafe_catchain_rotate(last_masterchain_seqno_, val_set->get_catchain_seqno());
          if (r) {
            td::uint8 b[36];
            td::MutableSlice x{b, 36};
            x.copy_from(val_group_id.as_slice());
            x.remove_prefix(32);
            CHECK(x.size() == 4);
            x.copy_from(td::Slice(reinterpret_cast<const td::uint8 *>(&r), 4));
            val_group_id = sha256_bits256(td::Slice(b, 36));
          }
        }

        VLOG(VALIDATOR_DEBUG) << "validating group " << val_group_id;
        auto it = validator_groups_.find(val_group_id);
        if (it != validator_groups_.end()) {
          new_validator_groups_.emplace(val_group_id, std::move(it->second));
        } else {
          auto it2 = next_validator_groups_.find(val_group_id);
          if (it2 != next_validator_groups_.end()) {
            if (!it2->second.actor.empty()) {
              td::actor::send_closure(it2->second.actor, &ValidatorGroup::start, prev, last_masterchain_block_id_);
            }
            new_validator_groups_.emplace(val_group_id, std::move(it2->second));
          } else {
            auto G = create_validator_group(val_group_id, shard, val_set, key_seqno, opts, started_);
            if (!G.empty()) {
              td::actor::send_closure(G, &ValidatorGroup::start, prev, last_masterchain_block_id_);
            }
            new_validator_groups_.emplace(val_group_id, ValidatorGroupEntry{std::move(G), shard});
          }
        }
      }
    }
  }
  for (auto &shard : future_shards) {
    auto val_set = last_masterchain_state_->get_next_validator_set(shard);
    if (val_set.is_null()) {
      continue;
    }

    auto validator_id = get_validator(shard, val_set);
    if (!validator_id.is_zero()) {
      auto val_group_id = get_validator_set_id(shard, val_set, opts_hash, key_seqno, opts);
      auto it = next_validator_groups_.find(val_group_id);
      if (it != next_validator_groups_.end()) {
        //CHECK(!it->second.empty());
        new_next_validator_groups_.emplace(val_group_id, std::move(it->second));
      } else {
        new_next_validator_groups_.emplace(
            val_group_id, ValidatorGroupEntry{
                              create_validator_group(val_group_id, shard, val_set, key_seqno, opts, started_), shard});
      }
    }
  }

  std::vector<td::actor::ActorId<ValidatorGroup>> gc;
  for (auto &v : validator_groups_) {
    if (!v.second.actor.empty()) {
      gc_list_.push_back(v.first);
      gc.push_back(v.second.actor.release());
    }
  }
  for (auto &v : next_validator_groups_) {
    if (!v.second.actor.empty()) {
      gc_list_.push_back(v.first);
      gc.push_back(v.second.actor.release());
    }
  }

  validator_groups_ = std::move(new_validator_groups_);
  next_validator_groups_ = std::move(new_next_validator_groups_);

  if (last_masterchain_state_->rotated_all_shards()) {
    gc_list_.clear();
    check_gc_list_.clear();
    CHECK(last_masterchain_block_handle_->received_state());
    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), gc = std::move(gc), block_id = last_masterchain_block_id_](td::Result<td::Unit> R) {
          R.ensure();
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::written_destroyed_validator_sessions, std::move(gc));
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::updated_init_block, block_id);
        });
    td::actor::send_closure(db_, &Db::update_init_masterchain_block, last_masterchain_block_id_, std::move(P));
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), gc = std::move(gc)](td::Result<td::Unit> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::written_destroyed_validator_sessions, std::move(gc));
    });
    td::actor::send_closure(db_, &Db::update_destroyed_validator_sessions, gc_list_, std::move(P));
  }
  if (!serializer_.empty()) {
    td::actor::send_closure(serializer_, &AsyncStateSerializer::auto_disable_serializer,
                            is_validator() && last_masterchain_state_->get_global_id() == -239);  // mainnet only
  }
}

void ValidatorManagerImpl::written_destroyed_validator_sessions(std::vector<td::actor::ActorId<ValidatorGroup>> list) {
  for (auto &v : list) {
    td::actor::send_closure(v, &ValidatorGroup::destroy);
  }
}

void ValidatorManagerImpl::update_shard_blocks() {
  {
    auto it = shard_blocks_.begin();
    while (it != shard_blocks_.end()) {
      auto &B = it->second;
      if (!B->may_be_valid(last_masterchain_block_handle_, last_masterchain_state_)) {
        auto it2 = it++;
        shard_blocks_.erase(it2);
      } else {
        ++it;
      }
    }
  }

  {
    auto it = out_shard_blocks_.begin();
    while (it != out_shard_blocks_.end()) {
      auto &B = it->second;
      if (!B->may_be_valid(last_masterchain_block_handle_, last_masterchain_state_)) {
        auto it2 = it++;
        out_shard_blocks_.erase(it2);
      } else {
        ++it;
      }
    }
  }
}

ValidatorSessionId ValidatorManagerImpl::get_validator_set_id(ShardIdFull shard, td::Ref<ValidatorSet> val_set,
                                                              td::Bits256 opts_hash, BlockSeqno last_key_block_seqno,
                                                              const validatorsession::ValidatorSessionOptions &opts) {
  std::vector<tl_object_ptr<ton_api::validator_groupMember>> vec;
  auto v = val_set->export_vector();
  auto vert_seqno = opts_->get_maximal_vertical_seqno();
  for (auto &n : v) {
    auto pub_key = PublicKey{pubkeys::Ed25519{n.key}};
    vec.push_back(
        create_tl_object<ton_api::validator_groupMember>(pub_key.compute_short_id().bits256_value(), n.addr, n.weight));
  }
  if (!opts.new_catchain_ids) {
    if (vert_seqno == 0) {
      return create_hash_tl_object<ton_api::validator_group>(shard.workchain, shard.shard,
                                                             val_set->get_catchain_seqno(), opts_hash, std::move(vec));
    } else {
      return create_hash_tl_object<ton_api::validator_groupEx>(
          shard.workchain, shard.shard, vert_seqno, val_set->get_catchain_seqno(), opts_hash, std::move(vec));
    }
  } else {
    return create_hash_tl_object<ton_api::validator_groupNew>(shard.workchain, shard.shard, vert_seqno,
                                                              last_key_block_seqno, val_set->get_catchain_seqno(),
                                                              opts_hash, std::move(vec));
  }
}

td::actor::ActorOwn<ValidatorGroup> ValidatorManagerImpl::create_validator_group(
    ValidatorSessionId session_id, ShardIdFull shard, td::Ref<ValidatorSet> validator_set, BlockSeqno key_seqno,
    validatorsession::ValidatorSessionOptions opts, bool init_session) {
  if (check_gc_list_.count(session_id) == 1) {
    return td::actor::ActorOwn<ValidatorGroup>{};
  } else {
    // Call get_external_messages to cleanup mempool for the shard
    get_external_messages(shard, [](td::Result<std::vector<std::pair<td::Ref<ExtMessage>, int>>>) {});

    auto validator_id = get_validator(shard, validator_set);
    CHECK(!validator_id.is_zero());
    auto G = td::actor::create_actor<ValidatorGroup>(
        PSTRING() << "valgroup" << shard.to_str(), shard, validator_id, session_id, validator_set, key_seqno, opts,
        keyring_, adnl_, rldp_,
        overlays_, db_root_, actor_id(this), init_session,
        opts_->check_unsafe_resync_allowed(validator_set->get_catchain_seqno()), opts_);
    return G;
  }
}

void ValidatorManagerImpl::add_handle_to_lru(BlockHandle handle) {
  auto it = handle_lru_map_.find(handle->id());
  if (it != handle_lru_map_.end()) {
    CHECK(it->second->handle() == handle);
    it->second->remove();
    handle_lru_.put(it->second.get());
  } else {
    auto id = handle->id();
    auto x = std::make_unique<BlockHandleLru>(std::move(handle));
    handle_lru_.put(x.get());
    handle_lru_map_.emplace(id, std::move(x));
    handle_lru_size_++;
    if (handle_lru_size_ > handle_lru_max_size_) {
      auto to_remove = BlockHandleLru::from_list_node(handle_lru_.get());
      CHECK(to_remove);
      CHECK(handle_lru_map_.count(to_remove->handle()->id()) == 1);
      handle_lru_map_.erase(to_remove->handle()->id());
      handle_lru_size_--;
    }
  }
}

BlockHandle ValidatorManagerImpl::get_handle_from_lru(BlockIdExt id) {
  auto it = handle_lru_map_.find(id);
  if (it != handle_lru_map_.end()) {
    it->second->remove();
    handle_lru_.put(it->second.get());
    auto handle = it->second->handle();
    CHECK(handle->id() == id);
    return handle;
  } else {
    return nullptr;
  }
}

void ValidatorManagerImpl::try_advance_gc_masterchain_block() {
  if (gc_masterchain_handle_ && last_masterchain_seqno_ > 0 && !gc_advancing_ &&
      gc_masterchain_handle_->inited_next_left() &&
      gc_masterchain_handle_->id().id.seqno < last_rotate_block_id_.id.seqno &&
      gc_masterchain_handle_->id().id.seqno < last_masterchain_state_->min_ref_masterchain_seqno() &&
      gc_masterchain_handle_->id().id.seqno + 1024 < last_masterchain_seqno_ &&
      gc_masterchain_handle_->id().id.seqno < last_masterchain_state_->last_key_block_id().seqno() &&
      gc_masterchain_handle_->id().id.seqno < min_confirmed_masterchain_seqno_ &&
      gc_masterchain_handle_->id().id.seqno < state_serializer_masterchain_seqno_ &&
      (double)gc_masterchain_state_->get_unix_time() < td::Clocks::system() - state_ttl()) {
    gc_advancing_ = true;
    auto block_id = gc_masterchain_handle_->one_next(true);

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::got_next_gc_masterchain_handle, R.move_as_ok());
    });
    get_block_handle(block_id, true, std::move(P));
  }
}

void ValidatorManagerImpl::allow_block_state_gc(BlockIdExt block_id, td::Promise<bool> promise) {
  if (!gc_masterchain_handle_) {
    promise.set_result(false);
    return;
  }
  if (block_id.is_masterchain()) {
    promise.set_result(block_id.id.seqno < gc_masterchain_handle_->id().id.seqno);
    return;
  }
  if (!gc_masterchain_state_->workchain_is_active(block_id.id.workchain)) {
    promise.set_result(false);
    return;
  }
  auto S = gc_masterchain_state_->get_shard_from_config(block_id.shard_full());
  if (S.not_null()) {
    promise.set_result(block_id.id.seqno < S->top_block_id().id.seqno);
    return;
  }
  auto shards = gc_masterchain_state_->get_shards();
  for (const auto &shard : shards) {
    if (shard_intersects(shard->shard(), block_id.shard_full())) {
      promise.set_result(block_id.id.seqno < shard->top_block_id().id.seqno);
      return;
    }
  }
  UNREACHABLE();
}

void ValidatorManagerImpl::got_next_gc_masterchain_handle(BlockHandle handle) {
  CHECK(gc_advancing_);
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle](td::Result<td::Ref<ShardState>> R) {
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::timeout) {
        LOG(ERROR) << "Failed to get gc masterchain state, retrying: " << R.move_as_error();
        td::actor::send_closure(SelfId, &ValidatorManagerImpl::got_next_gc_masterchain_handle, std::move(handle));
      } else {
        LOG(FATAL) << "Failed to get gc masterchain state: " << R.move_as_error();
      }
      return;
    }
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::got_next_gc_masterchain_state, std::move(handle),
                            td::Ref<MasterchainState>{R.move_as_ok()});
  });
  wait_block_state(handle, 0, td::Timestamp::in(60.0), std::move(P));
}

void ValidatorManagerImpl::got_next_gc_masterchain_state(BlockHandle handle, td::Ref<MasterchainState> state) {
  auto P = td::PromiseCreator::lambda([handle, state, SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::advance_gc, handle, state);
  });
  update_gc_block_handle(std::move(handle), std::move(P));
}

void ValidatorManagerImpl::update_gc_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::update_gc_masterchain_block, handle->id(), std::move(promise));
}

void ValidatorManagerImpl::advance_gc(BlockHandle handle, td::Ref<MasterchainState> state) {
  CHECK(gc_advancing_);
  gc_advancing_ = false;
  gc_masterchain_handle_ = std::move(handle);
  gc_masterchain_state_ = std::move(state);
  try_advance_gc_masterchain_block();
}

void ValidatorManagerImpl::update_shard_client_block_handle(BlockHandle handle, td::Ref<MasterchainState> state,
                                                            td::Promise<td::Unit> promise) {
  shard_client_handle_ = std::move(handle);
  auto seqno = shard_client_handle_->id().seqno();
  if (state.not_null()) {
    shard_client_shards_ = state->get_shards();
    if (last_liteserver_state_.is_null() || last_liteserver_state_->get_block_id().seqno() < seqno) {
      last_liteserver_state_ = std::move(state);
    }
  }
  shard_client_update(seqno);
  promise.set_value(td::Unit());
}

void ValidatorManagerImpl::shard_client_update(BlockSeqno seqno) {
  if (min_confirmed_masterchain_seqno_ < seqno) {
    min_confirmed_masterchain_seqno_ = seqno;
  } else {
    return;
  }
  while (shard_client_waiters_.size() > 0) {
    auto it = shard_client_waiters_.begin();
    if (it->first > min_confirmed_masterchain_seqno_) {
      break;
    }
    for (auto &y : it->second.waiting_) {
      y.promise.set_value(td::Unit());
    }
    shard_client_waiters_.erase(it);
  }
}

void ValidatorManagerImpl::state_serializer_update(BlockSeqno seqno) {
  if (state_serializer_masterchain_seqno_ < seqno) {
    state_serializer_masterchain_seqno_ = seqno;
  }
}

void ValidatorManagerImpl::alarm() {
  try_advance_gc_masterchain_block();
  alarm_timestamp() = td::Timestamp::in(1.0);
  if (shard_client_handle_ && gc_masterchain_handle_) {
    td::actor::send_closure(db_, &Db::run_gc, shard_client_handle_->unix_time(), gc_masterchain_handle_->unix_time(),
                            opts_->archive_ttl());
  }
  if (log_status_at_.is_in_past()) {
    if (last_masterchain_block_handle_) {
      LOG(ERROR) << "STATUS: last_masterchain_block_ago="
                 << td::format::as_time(td::Clocks::system() - last_masterchain_block_handle_->unix_time())
                 << " last_known_key_block_ago="
                 << td::format::as_time(td::Clocks::system() - (last_known_key_block_handle_->inited_unix_time()
                                                                    ? last_known_key_block_handle_->unix_time()
                                                                    : 0))
                 << " shard_client_ago="
                 << td::format::as_time(td::Clocks::system() -
                                        (shard_client_handle_ ? shard_client_handle_->unix_time() : 0));
    }
    log_status_at_ = td::Timestamp::in(60.0);
  }
  alarm_timestamp().relax(log_status_at_);
  if (resend_shard_blocks_at_ && resend_shard_blocks_at_.is_in_past()) {
    resend_shard_blocks_at_ = td::Timestamp::never();
    for (auto &B : out_shard_blocks_) {
      callback_->send_shard_block_info(B.second->block_id(), B.second->catchain_seqno(), B.second->serialize());
    }
    if (out_shard_blocks_.size() > 0) {
      resend_shard_blocks_at_ = td::Timestamp::in(td::Random::fast(0, 100) * 0.01 + 2);
    }
  }
  alarm_timestamp().relax(resend_shard_blocks_at_);
  if (check_waiters_at_.is_in_past()) {
    check_waiters_at_ = td::Timestamp::in(1.0);
    for (auto &w : wait_block_data_) {
      w.second.check_timers();
    }
    for (auto &w : wait_state_) {
      w.second.check_timers();
    }
    for (auto &w : shard_client_waiters_) {
      w.second.check_timers();
    }
    for (auto it = block_state_cache_.begin(); it != block_state_cache_.end();) {
      bool del = it->second.ttl_.is_in_past();
      if (del) {
        auto block_id = it->first;
        if (block_id.is_masterchain()) {
          if (block_id.seqno() == last_masterchain_seqno_) {
            it->second.ttl_ = td::Timestamp::in(30.0);
            del = false;
          }
        } else if (last_masterchain_state_.not_null()) {
          auto shard = last_masterchain_state_->get_shard_from_config(block_id.shard_full());
          if (shard.not_null()) {
            if (block_id.seqno() == shard->top_block_id().seqno()) {
              it->second.ttl_ = td::Timestamp::in(30.0);
              del = false;
            }
          }
        }
      }
      if (del) {
        it = block_state_cache_.erase(it);
      } else {
        ++it;
      }
    }
  }
  alarm_timestamp().relax(check_waiters_at_);
  if (check_shard_clients_.is_in_past()) {
    check_shard_clients_ = td::Timestamp::in(10.0);

    if (!serializer_.empty()) {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockSeqno> R) {
        if (R.is_error()) {
          VLOG(VALIDATOR_WARNING) << "failed to get shard client status: " << R.move_as_error();
        } else {
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::state_serializer_update, R.move_as_ok());
        }
      });
      td::actor::send_closure(serializer_, &AsyncStateSerializer::get_masterchain_seqno, std::move(P));
    }
  }
  alarm_timestamp().relax(check_shard_clients_);

  if (log_ls_stats_at_.is_in_past()) {
    if (!ls_stats_.empty() || ls_stats_check_ext_messages_ != 0) {
      td::StringBuilder sb;
      sb << "Liteserver stats (1 minute):";
      td::uint32 total = 0;
      for (const auto &p : ls_stats_) {
        sb << " " << lite_query_name_by_id(p.first) << ":" << p.second;
        total += p.second;
      }
      if (total > 0) {
        sb << " TOTAL:" << total;
      }
      if (ls_stats_check_ext_messages_ > 0) {
        sb << " checkExtMessage:" << ls_stats_check_ext_messages_;
      }
      LOG(WARNING) << sb.as_cslice();
    }
    ls_stats_.clear();
    ls_stats_check_ext_messages_ = 0;
    log_ls_stats_at_ = td::Timestamp::in(60.0);
  }
  alarm_timestamp().relax(log_ls_stats_at_);
  if (cleanup_mempool_at_.is_in_past()) {
    if (is_validator()) {
      get_external_messages(ShardIdFull{masterchainId, shardIdAll},
                            [](td::Result<std::vector<std::pair<td::Ref<ExtMessage>, int>>>) {});
      get_external_messages(ShardIdFull{basechainId, shardIdAll},
                            [](td::Result<std::vector<std::pair<td::Ref<ExtMessage>, int>>>) {});
    }
    cleanup_mempool_at_ = td::Timestamp::in(250.0);
  }
  alarm_timestamp().relax(cleanup_mempool_at_);
}

void ValidatorManagerImpl::update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::update_shard_client_state, masterchain_block_id, std::move(promise));
}

void ValidatorManagerImpl::get_shard_client_state(bool from_db, td::Promise<BlockIdExt> promise) {
  if (shard_client_handle_ && !from_db) {
    promise.set_result(shard_client_handle_->id());
  } else {
    td::actor::send_closure(db_, &Db::get_shard_client_state, std::move(promise));
  }
}

void ValidatorManagerImpl::update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::update_async_serializer_state, std::move(state), std::move(promise));
}

void ValidatorManagerImpl::get_async_serializer_state(td::Promise<AsyncSerializerState> promise) {
  td::actor::send_closure(db_, &Db::get_async_serializer_state, std::move(promise));
}

void ValidatorManagerImpl::try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::try_get_static_file, file_hash, std::move(promise));
}

void ValidatorManagerImpl::get_archive_id(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix,
                                          td::Promise<td::uint64> promise) {
  if (masterchain_seqno > last_masterchain_seqno_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "masterchain seqno too big"));
    return;
  }
  td::actor::send_closure(db_, &Db::get_archive_id, masterchain_seqno, shard_prefix, std::move(promise));
}

void ValidatorManagerImpl::get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                                             td::Promise<td::BufferSlice> promise) {
  td::actor::send_closure(db_, &Db::get_archive_slice, archive_id, offset, limit, std::move(promise));
}

bool ValidatorManagerImpl::is_validator() {
  return temp_keys_.size() > 0 || permanent_keys_.size() > 0;
}

bool ValidatorManagerImpl::validating_masterchain() {
  return !get_validator(ShardIdFull(masterchainId),
                        last_masterchain_state_->get_validator_set(ShardIdFull(masterchainId)))
              .is_zero();
}

PublicKeyHash ValidatorManagerImpl::get_validator(ShardIdFull shard, td::Ref<ValidatorSet> val_set) {
  for (auto &key : temp_keys_) {
    if (val_set->is_validator(key.bits256_value())) {
      return key;
    }
  }
  return PublicKeyHash::zero();
}

void ValidatorManagerImpl::got_next_key_blocks(std::vector<BlockIdExt> r) {
  if (r.size() == 0) {
    delay_action([SelfId = actor_id(
                      this)]() { td::actor::send_closure(SelfId, &ValidatorManagerImpl::send_peek_key_block_request); },
                 td::Timestamp::in(2.0 + td::Random::fast(0, 100) * 0.01));
    return;
  }
  auto block_id = *r.rbegin();
  if (block_id.seqno() <= last_known_key_block_handle_->id().seqno()) {
    delay_action([SelfId = actor_id(
                      this)]() { td::actor::send_closure(SelfId, &ValidatorManagerImpl::send_peek_key_block_request); },
                 td::Timestamp::in(2.0 + td::Random::fast(0, 100) * 0.01));
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &ValidatorManagerImpl::update_last_known_key_block, R.move_as_ok(), true);
  });
  get_block_handle(block_id, false, std::move(P));
}

void ValidatorManagerImpl::update_last_known_key_block(BlockHandle handle, bool send_request) {
  if (last_known_key_block_handle_ && handle->id().seqno() > last_known_key_block_handle_->id().seqno()) {
    last_known_key_block_handle_ = std::move(handle);
    callback_->new_key_block(last_known_key_block_handle_);
  }
  if (send_request) {
    delay_action([SelfId = actor_id(
                      this)]() { td::actor::send_closure(SelfId, &ValidatorManagerImpl::send_peek_key_block_request); },
                 td::Timestamp::in(0.1 + td::Random::fast(0, 100) * 0.001));
  }
}

void ValidatorManagerImpl::send_peek_key_block_request() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<BlockIdExt>> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::got_next_key_blocks, std::vector<BlockIdExt>{});
    } else {
      td::actor::send_closure(SelfId, &ValidatorManagerImpl::got_next_key_blocks, R.move_as_ok());
    }
  });

  send_get_next_key_blocks_request(last_known_key_block_handle_->id(), 1, std::move(P));
}

void ValidatorManagerImpl::prepare_actor_stats(td::Promise<std::string> promise) {
  send_closure(actor_stats_, &td::actor::ActorStats::prepare_stats, std::move(promise));
}

void ValidatorManagerImpl::prepare_stats(td::Promise<std::vector<std::pair<std::string, std::string>>> promise) {
  auto merger = StatsMerger::create(std::move(promise));

  std::vector<std::pair<std::string, std::string>> vec;
  vec.emplace_back("unixtime", td::to_string(static_cast<UnixTime>(td::Clocks::system())));
  if (last_masterchain_block_handle_) {
    vec.emplace_back("masterchainblock", last_masterchain_block_id_.to_str());
    vec.emplace_back("masterchainblocktime", td::to_string(last_masterchain_block_handle_->unix_time()));
    vec.emplace_back("gcmasterchainblock", gc_masterchain_handle_->id().to_str());
    vec.emplace_back("keymasterchainblock", last_key_block_handle_->id().to_str());
    vec.emplace_back("knownkeymasterchainblock", last_known_key_block_handle_->id().to_str());
    vec.emplace_back("rotatemasterchainblock", last_rotate_block_id_.to_str());
    //vec.emplace_back("shardclientmasterchainseqno", td::to_string(min_confirmed_masterchain_seqno_));
  }

  td::NamedThreadSafeCounter::get_default().for_each([&](auto key, auto value) {
    vec.emplace_back("counter." + key, PSTRING() << value);
  });

  if (!shard_client_.empty()) {
    auto P = td::PromiseCreator::lambda([promise = merger.make_promise("")](td::Result<BlockSeqno> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
        return;
      }
      std::vector<std::pair<std::string, std::string>> vec;
      vec.emplace_back("shardclientmasterchainseqno", td::to_string(R.move_as_ok()));
      promise.set_value(std::move(vec));
    });
    td::actor::send_closure(shard_client_, &ShardClient::get_processed_masterchain_block, std::move(P));
  }

  vec.emplace_back("start_time", td::to_string(started_at_));
  for (int iter = 0; iter < 2; ++iter) {
    td::StringBuilder sb;
    td::uint32 total = 0;
    for (const auto &p : (iter ? total_ls_queries_error_ : total_ls_queries_ok_)) {
      sb << lite_query_name_by_id(p.first) << ":" << p.second << " ";
      total += p.second;
    }
    sb << "TOTAL:" << total;
    vec.emplace_back(PSTRING() << "total.ls_queries_" << (iter ? "error" : "ok"), sb.as_cslice().str());
  }
  vec.emplace_back("total.ext_msg_check",
                   PSTRING() << "ok:" << total_check_ext_messages_ok_ << " error:" << total_check_ext_messages_error_);
  vec.emplace_back("total.collated_blocks.master", PSTRING() << "ok:" << total_collated_blocks_master_ok_
                                                             << " error:" << total_collated_blocks_master_error_);
  vec.emplace_back("total.collated_blocks.shard", PSTRING() << "ok:" << total_collated_blocks_shard_ok_
                                                            << " error:" << total_collated_blocks_shard_error_);
  vec.emplace_back("total.validated_blocks.master", PSTRING() << "ok:" << total_validated_blocks_master_ok_
                                                              << " error:" << total_validated_blocks_master_error_);
  vec.emplace_back("total.validated_blocks.shard", PSTRING() << "ok:" << total_validated_blocks_shard_ok_
                                                             << " error:" << total_validated_blocks_shard_error_);
  if (is_validator()) {
    vec.emplace_back("active_validator_groups", PSTRING() << "master:" << active_validator_groups_master_
                                                          << " shard:" << active_validator_groups_shard_);
  }

  bool serializer_enabled = opts_->get_state_serializer_enabled();
  if (is_validator() && last_masterchain_state_.not_null() && last_masterchain_state_->get_global_id() == -239) {
    serializer_enabled = false;
  }
  vec.emplace_back("stateserializerenabled", serializer_enabled ? "true" : "false");

  merger.make_promise("").set_value(std::move(vec));

  if (!serializer_.empty()) {
    td::actor::send_closure(serializer_, &AsyncStateSerializer::prepare_stats, merger.make_promise(""));
  }

  td::actor::send_closure(db_, &Db::prepare_stats, merger.make_promise("db."));
  for (auto &[_, p] : stats_providers_) {
    p.second(merger.make_promise(p.first));
  }
}

void ValidatorManagerImpl::prepare_perf_timer_stats(td::Promise<std::vector<PerfTimerStats>> promise) {
  promise.set_value(std::vector<PerfTimerStats>(perf_timer_stats));
}

void ValidatorManagerImpl::add_perf_timer_stat(std::string name, double duration) {
  for (auto &s : perf_timer_stats) {
    if (s.name == name) {
      double now = td::Time::now();
      while (!s.stats.empty() && s.stats.front().first < now - 3600.0) {
        s.stats.pop_front();
      }
      s.stats.push_back({td::Time::now(), duration});
      return;
    }
  }
  perf_timer_stats.push_back({name, {{td::Time::now(), duration}}});
}

void ValidatorManagerImpl::truncate(BlockSeqno seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) {
  td::actor::send_closure(db_, &Db::truncate, seqno, std::move(handle), std::move(promise));
}

void ValidatorManagerImpl::wait_shard_client_state(BlockSeqno seqno, td::Timestamp timeout,
                                                   td::Promise<td::Unit> promise) {
  if (seqno <= min_confirmed_masterchain_seqno_) {
    promise.set_value(td::Unit());
    return;
  }
  if (timeout.is_in_past()) {
    promise.set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
    return;
  }
  if (seqno > min_confirmed_masterchain_seqno_ + 100) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "too big masterchain block seqno"));
    return;
  }

  shard_client_waiters_[seqno].waiting_.emplace_back(timeout, 0, std::move(promise));
}

void ValidatorManagerImpl::log_validator_session_stats(BlockIdExt block_id,
                                                       validatorsession::ValidatorSessionStats stats) {
  std::string fname = opts_->get_session_logs_file();
  if (fname.empty()) {
    return;
  }

  std::vector<tl_object_ptr<ton_api::validatorSession_statsRound>> rounds;
  for (const auto &round : stats.rounds) {
    std::vector<tl_object_ptr<ton_api::validatorSession_statsProducer>> producers;
    for (const auto &producer : round.producers) {
      BlockIdExt cur_block_id{block_id.id, producer.root_hash, producer.file_hash};
      auto it = recorded_block_stats_.find(cur_block_id);
      tl_object_ptr<ton_api::validatorSession_collationStats> collation_stats;
      if (it != recorded_block_stats_.end() && it->second.collator_stats_) {
        auto &stats = it->second.collator_stats_.value();
        collation_stats = create_tl_object<ton_api::validatorSession_collationStats>(
            stats.bytes, stats.gas, stats.lt_delta, stats.cat_bytes, stats.cat_gas, stats.cat_lt_delta,
            stats.limits_log, stats.ext_msgs_total, stats.ext_msgs_filtered, stats.ext_msgs_accepted,
            stats.ext_msgs_rejected);
      }
      std::string approvers, signers;
      for (bool x : producer.approvers) {
        approvers += (x ? '1' : '0');
      }
      for (bool x : producer.signers) {
        signers += (x ? '1' : '0');
      }
      producers.push_back(create_tl_object<ton_api::validatorSession_statsProducer>(
          producer.id.bits256_value(), producer.candidate_id, producer.block_status, producer.root_hash,
          producer.file_hash, producer.comment, producer.block_timestamp, producer.is_accepted, producer.is_ours,
          producer.got_submit_at, producer.collation_time, producer.collated_at, producer.collation_cached,
          it == recorded_block_stats_.end() ? -1.0 : it->second.collator_work_time_,
          it == recorded_block_stats_.end() ? -1.0 : it->second.collator_cpu_work_time_, std::move(collation_stats),
          producer.validation_time, producer.validated_at, producer.validation_cached,
          it == recorded_block_stats_.end() ? -1.0 : it->second.validator_work_time_,
          it == recorded_block_stats_.end() ? -1.0 : it->second.validator_cpu_work_time_, producer.gen_utime,
          producer.approved_weight, producer.approved_33pct_at, producer.approved_66pct_at, std::move(approvers),
          producer.signed_weight, producer.signed_33pct_at, producer.signed_66pct_at, std::move(signers),
          producer.serialize_time, producer.deserialize_time, producer.serialized_size));
    }
    rounds.push_back(create_tl_object<ton_api::validatorSession_statsRound>(round.timestamp, std::move(producers)));
  }

  auto obj = create_tl_object<ton_api::validatorSession_stats>(
      stats.success, create_tl_block_id(block_id), stats.timestamp, stats.self.bits256_value(), stats.session_id,
      stats.cc_seqno, stats.creator.bits256_value(), stats.total_validators, stats.total_weight, stats.signatures,
      stats.signatures_weight, stats.approve_signatures, stats.approve_signatures_weight, stats.first_round,
      std::move(rounds));
  auto s = td::json_encode<std::string>(td::ToJson(*obj.get()), false);
  s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\n' || c == '\r'; }), s.end());

  std::ofstream file;
  file.open(fname, std::ios_base::app);
  file << s << "\n";
  file.close();

  LOG(INFO) << "Writing validator session stats for " << block_id.id.to_str();
}

void ValidatorManagerImpl::log_new_validator_group_stats(validatorsession::NewValidatorGroupStats stats) {
  std::string fname = opts_->get_session_logs_file();
  if (fname.empty()) {
    return;
  }
  std::vector<tl_object_ptr<ton_api::validatorSession_newValidatorGroupStats_node>> nodes;
  for (const auto &node : stats.nodes) {
    nodes.push_back(
        create_tl_object<ton_api::validatorSession_newValidatorGroupStats_node>(node.id.bits256_value(), node.weight));
  }
  auto obj = create_tl_object<ton_api::validatorSession_newValidatorGroupStats>(
      stats.session_id, stats.shard.workchain, stats.shard.shard, stats.cc_seqno, stats.last_key_block_seqno,
      stats.timestamp, stats.self_idx, std::move(nodes));
  auto s = td::json_encode<std::string>(td::ToJson(*obj.get()), false);
  s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\n' || c == '\r'; }), s.end());

  std::ofstream file;
  file.open(fname, std::ios_base::app);
  file << s << "\n";
  file.close();

  LOG(INFO) << "Writing new validator group stats for " << stats.session_id << " shard=" << stats.shard.to_str()
            << " cc_seqno=" << stats.cc_seqno;
}

void ValidatorManagerImpl::log_end_validator_group_stats(validatorsession::EndValidatorGroupStats stats) {
  std::string fname = opts_->get_session_logs_file();
  if (fname.empty()) {
    return;
  }
  std::vector<tl_object_ptr<ton_api::validatorSession_endValidatorGroupStats_node>> nodes;
  for (const auto &node : stats.nodes) {
    nodes.push_back(create_tl_object<ton_api::validatorSession_endValidatorGroupStats_node>(node.id.bits256_value(),
                                                                                            node.catchain_blocks));
  }
  auto obj = create_tl_object<ton_api::validatorSession_endValidatorGroupStats>(stats.session_id, stats.timestamp,
                                                                                std::move(nodes));
  auto s = td::json_encode<std::string>(td::ToJson(*obj.get()), false);
  s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\n' || c == '\r'; }), s.end());

  std::ofstream file;
  file.open(fname, std::ios_base::app);
  file << s << "\n";
  file.close();

  LOG(INFO) << "Writing end validator group stats for " << stats.session_id;
}

void ValidatorManagerImpl::get_block_handle_for_litequery(BlockIdExt block_id, td::Promise<ConstBlockHandle> promise) {
  get_block_handle(block_id, false,
                   [SelfId = actor_id(this), block_id, promise = std::move(promise),
                    allow_not_applied = opts_->nonfinal_ls_queries_enabled()](td::Result<BlockHandle> R) mutable {
                     if (R.is_ok() && (allow_not_applied || R.ok()->is_applied())) {
                       promise.set_value(R.move_as_ok());
                     } else {
                       td::actor::send_closure(SelfId, &ValidatorManagerImpl::process_block_handle_for_litequery_error,
                                               block_id, std::move(R), std::move(promise));
                     }
                   });
}

void ValidatorManagerImpl::get_block_data_for_litequery(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) {
  if (candidates_buffer_.empty()) {
    get_block_handle_for_litequery(
        block_id, [manager = actor_id(this), promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
          TRY_RESULT_PROMISE(promise, handle, std::move(R));
          td::actor::send_closure_later(manager, &ValidatorManager::get_block_data_from_db, std::move(handle),
                                        std::move(promise));
        });
  } else {
    td::actor::send_closure(
        candidates_buffer_, &CandidatesBuffer::get_block_data, block_id,
        [manager = actor_id(this), promise = std::move(promise), block_id](td::Result<td::Ref<BlockData>> R) mutable {
          if (R.is_ok()) {
            promise.set_result(R.move_as_ok());
            return;
          }
          td::actor::send_closure(manager, &ValidatorManagerImpl::get_block_handle_for_litequery, block_id,
                                  [manager, promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
                                    TRY_RESULT_PROMISE(promise, handle, std::move(R));
                                    td::actor::send_closure_later(manager, &ValidatorManager::get_block_data_from_db,
                                                                  std::move(handle), std::move(promise));
                                  });
        });
  }
}

void ValidatorManagerImpl::get_block_state_for_litequery(BlockIdExt block_id,
                                                         td::Promise<td::Ref<ShardState>> promise) {
  if (candidates_buffer_.empty()) {
    get_block_handle_for_litequery(
        block_id, [manager = actor_id(this), promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
          TRY_RESULT_PROMISE(promise, handle, std::move(R));
          td::actor::send_closure_later(manager, &ValidatorManager::get_shard_state_from_db, std::move(handle),
                                        std::move(promise));
        });
  } else {
    td::actor::send_closure(
        candidates_buffer_, &CandidatesBuffer::get_block_state, block_id,
        [manager = actor_id(this), promise = std::move(promise), block_id](td::Result<td::Ref<ShardState>> R) mutable {
          if (R.is_ok()) {
            promise.set_result(R.move_as_ok());
            return;
          }
          td::actor::send_closure(manager, &ValidatorManagerImpl::get_block_handle_for_litequery, block_id,
                                  [manager, promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
                                    TRY_RESULT_PROMISE(promise, handle, std::move(R));
                                    td::actor::send_closure_later(manager, &ValidatorManager::get_shard_state_from_db,
                                                                  std::move(handle), std::move(promise));
                                  });
        });
  }
}

void ValidatorManagerImpl::get_block_by_lt_for_litequery(AccountIdPrefixFull account, LogicalTime lt,
                                                         td::Promise<ConstBlockHandle> promise) {
  get_block_by_lt_from_db(
      account, lt, [=, SelfId = actor_id(this), promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
        if (R.is_ok() && R.ok()->is_applied()) {
          promise.set_value(R.move_as_ok());
        } else {
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::process_lookup_block_for_litequery_error, account, 0,
                                  lt, std::move(R), std::move(promise));
        }
      });
}

void ValidatorManagerImpl::get_block_by_unix_time_for_litequery(AccountIdPrefixFull account, UnixTime ts,
                                                                td::Promise<ConstBlockHandle> promise) {
  get_block_by_unix_time_from_db(
      account, ts, [=, SelfId = actor_id(this), promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
        if (R.is_ok() && R.ok()->is_applied()) {
          promise.set_value(R.move_as_ok());
        } else {
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::process_lookup_block_for_litequery_error, account, 1,
                                  ts, std::move(R), std::move(promise));
        }
      });
}

void ValidatorManagerImpl::get_block_by_seqno_for_litequery(AccountIdPrefixFull account, BlockSeqno seqno,
                                                            td::Promise<ConstBlockHandle> promise) {
  get_block_by_seqno_from_db(
      account, seqno,
      [=, SelfId = actor_id(this), promise = std::move(promise)](td::Result<ConstBlockHandle> R) mutable {
        if (R.is_ok() && R.ok()->is_applied()) {
          promise.set_value(R.move_as_ok());
        } else {
          td::actor::send_closure(SelfId, &ValidatorManagerImpl::process_lookup_block_for_litequery_error, account, 2,
                                  seqno, std::move(R), std::move(promise));
        }
      });
}

void ValidatorManagerImpl::process_block_handle_for_litequery_error(BlockIdExt block_id,
                                                                    td::Result<BlockHandle> r_handle,
                                                                    td::Promise<ConstBlockHandle> promise) {
  td::Status err;
  if (r_handle.is_error()) {
    err = r_handle.move_as_error();
  } else {
    auto handle = r_handle.move_as_ok();
    if (handle->is_applied()) {
      promise.set_value(std::move(handle));
      return;
    }
    if (!handle->received() || !handle->received_state()) {
      err = td::Status::Error(ErrorCode::notready, PSTRING() << "block " << block_id.id.to_str() << " is not in db");
    } else {
      err = td::Status::Error(ErrorCode::notready, PSTRING() << "block " << block_id.id.to_str() << " is not applied");
    }
  }
  if (block_id.is_masterchain()) {
    if (block_id.seqno() > last_masterchain_seqno_) {
      err = err.move_as_error_suffix(PSTRING() << " (last known masterchain block: " << last_masterchain_seqno_ << ")");
    }
  } else {
    for (auto &shard : shard_client_shards_) {
      if (shard_intersects(shard->shard(), block_id.shard_full())) {
        if (block_id.seqno() > shard->top_block_id().seqno()) {
          err = err.move_as_error_suffix(
              PSTRING() << " (possibly out of sync: shard_client_seqno="
                        << (shard_client_handle_ ? shard_client_handle_->id().seqno() : 0) << " ls_seqno="
                        << (last_liteserver_state_.not_null() ? last_liteserver_state_->get_seqno() : 0) << ")");
        }
        break;
      }
    }
  }
  promise.set_error(std::move(err));
}

void ValidatorManagerImpl::process_lookup_block_for_litequery_error(AccountIdPrefixFull account, int type,
                                                                    td::uint64 value,
                                                                    td::Result<ConstBlockHandle> r_handle,
                                                                    td::Promise<ConstBlockHandle> promise) {
  td::Status err;
  if (r_handle.is_error()) {
    err = r_handle.move_as_error();
  } else {
    auto handle = r_handle.move_as_ok();
    if (handle->is_applied()) {
      promise.set_value(std::move(handle));
      return;
    }
    if (!handle->received() || !handle->received_state()) {
      err = td::Status::Error(ErrorCode::notready, PSTRING() << "block " << handle->id().to_str() << " is not in db");
    } else {
      err = td::Status::Error(ErrorCode::notready, PSTRING() << "block " << handle->id().to_str() << " is not applied");
    }
  }
  if (account.is_masterchain()) {
    if (value > (type == 0
                     ? last_masterchain_state_->get_logical_time()
                     : (type == 1 ? last_masterchain_state_->get_unix_time() : last_masterchain_state_->get_seqno()))) {
      err = err.move_as_error_suffix(PSTRING() << " (last known masterchain block: " << last_masterchain_seqno_ << ")");
    }
  } else {
    for (auto &shard : shard_client_shards_) {
      if (shard_intersects(shard->shard(), account.as_leaf_shard())) {
        if (value > (type == 0 ? shard->end_lt()
                               : (type == 1 ? (shard_client_handle_ ? shard_client_handle_->unix_time() : 0)
                                            : shard->top_block_id().seqno()))) {
          err = err.move_as_error_suffix(
              PSTRING() << " (possibly out of sync: shard_client_seqno="
                        << (shard_client_handle_ ? shard_client_handle_->id().seqno() : 0) << " ls_seqno="
                        << (last_liteserver_state_.not_null() ? last_liteserver_state_->get_seqno() : 0) << ")");
        }
        break;
      }
    }
  }
  static std::string names[3] = {"lt", "utime", "seqno"};
  err = err.move_as_error_prefix(PSTRING() << "cannot find block " << account.to_str() << " " << names[type] << "="
                                           << value << ": ");
  promise.set_error(std::move(err));
}

void ValidatorManagerImpl::get_block_candidate_for_litequery(PublicKey source, BlockIdExt block_id,
                                                             FileHash collated_data_hash,
                                                             td::Promise<BlockCandidate> promise) {
  if (!opts_->nonfinal_ls_queries_enabled()) {
    promise.set_error(td::Status::Error("query is not allowed"));
    return;
  }
  get_block_candidate_from_db(source, block_id, collated_data_hash, std::move(promise));
}

void ValidatorManagerImpl::get_validator_groups_info_for_litequery(
    td::optional<ShardIdFull> shard,
    td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroups>> promise) {
  if (!opts_->nonfinal_ls_queries_enabled()) {
    promise.set_error(td::Status::Error("query is not allowed"));
    return;
  }
  class Actor : public td::actor::Actor {
   public:
    explicit Actor(std::vector<td::actor::ActorId<ValidatorGroup>> groups,
                   td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroups>> promise)
        : groups_(std::move(groups)), promise_(std::move(promise)) {
    }

    void start_up() override {
      pending_ = groups_.size();
      if (pending_ == 0) {
        promise_.set_result(std::move(result_));
        stop();
        return;
      }
      for (auto &x : groups_) {
        td::actor::send_closure(
            x, &ValidatorGroup::get_validator_group_info_for_litequery,
            [SelfId = actor_id(this)](td::Result<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo>> R) {
              td::actor::send_closure(SelfId, &Actor::on_result, R.is_ok() ? R.move_as_ok() : nullptr);
            });
      }
    }

    void on_result(tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo> r) {
      if (r) {
        result_->groups_.push_back(std::move(r));
      }
      --pending_;
      if (pending_ == 0) {
        promise_.set_result(std::move(result_));
        stop();
      }
    }

   private:
    std::vector<td::actor::ActorId<ValidatorGroup>> groups_;
    size_t pending_;
    td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroups>> promise_;
    tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroups> result_ =
        create_tl_object<lite_api::liteServer_nonfinal_validatorGroups>();
  };
  std::vector<td::actor::ActorId<ValidatorGroup>> groups;
  for (auto &x : validator_groups_) {
    if (x.second.actor.empty()) {
      continue;
    }
    if (shard && shard.value() != x.second.shard) {
      continue;
    }
    groups.push_back(x.second.actor.get());
  }
  td::actor::create_actor<Actor>("get-validator-groups-info", std::move(groups), std::move(promise)).release();
}

void ValidatorManagerImpl::update_options(td::Ref<ValidatorManagerOptions> opts) {
  if (!shard_client_.empty()) {
    td::actor::send_closure(shard_client_, &ShardClient::update_options, opts);
  }
  if (!serializer_.empty()) {
    td::actor::send_closure(serializer_, &AsyncStateSerializer::update_options, opts);
  }
  if (!queue_size_counter_.empty()) {
    td::actor::send_closure(queue_size_counter_, &QueueSizeCounter::update_options, opts);
  }
  for (auto &group : validator_groups_) {
    td::actor::send_closure(group.second.actor, &ValidatorGroup::update_options, opts);
  }
  for (auto &group : next_validator_groups_) {
    td::actor::send_closure(group.second.actor, &ValidatorGroup::update_options, opts);
  }
  opts_ = std::move(opts);
}

void ValidatorManagerImpl::add_persistent_state_description(td::Ref<PersistentStateDescription> desc) {
  auto now = (UnixTime)td::Clocks::system();
  if (desc->end_time <= now) {
    return;
  }
  td::actor::send_closure(db_, &Db::add_persistent_state_description, desc, [](td::Result<td::Unit>) {});
  auto it = persistent_state_descriptions_.begin();
  while (it != persistent_state_descriptions_.end()) {
    const auto &prev_desc = it->second;
    if (prev_desc->end_time <= now) {
      for (const BlockIdExt &block_id : prev_desc->shard_blocks) {
        persistent_state_blocks_.erase(block_id);
      }
      it = persistent_state_descriptions_.erase(it);
    } else {
      ++it;
    }
  }
  add_persistent_state_description_impl(std::move(desc));
}

void ValidatorManagerImpl::add_persistent_state_description_impl(td::Ref<PersistentStateDescription> desc) {
  if (!persistent_state_descriptions_.emplace(desc->masterchain_id.seqno(), desc).second) {
    return;
  }
  LOG(DEBUG) << "Add persistent state description for mc block " << desc->masterchain_id.to_str()
             << " start_time=" << desc->start_time << " end_time=" << desc->end_time;
  for (const BlockIdExt &block_id : desc->shard_blocks) {
    persistent_state_blocks_[block_id] = desc;
    LOG(DEBUG) << "Persistent state description: shard block " << block_id.to_str();
  }
}

void ValidatorManagerImpl::got_persistent_state_descriptions(std::vector<td::Ref<PersistentStateDescription>> descs) {
  for (auto &desc : descs) {
    add_persistent_state_description_impl(std::move(desc));
  }
}

td::Ref<PersistentStateDescription> ValidatorManagerImpl::get_block_persistent_state_to_download(BlockIdExt block_id) {
  if (block_id.is_masterchain()) {
    return {};
  }
  auto it = persistent_state_blocks_.find(block_id);
  if (it == persistent_state_blocks_.end()) {
    return {};
  }
  if (it->second->masterchain_id.seqno() + 16 >= min_confirmed_masterchain_seqno_) {
    // Do not download persistent states during ordinary shard client sync
    return {};
  }
  return it->second;
}

td::actor::ActorOwn<ValidatorManagerInterface> ValidatorManagerFactory::create(
    td::Ref<ValidatorManagerOptions> opts, std::string db_root, td::actor::ActorId<keyring::Keyring> keyring,
    td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
    td::actor::ActorId<overlay::Overlays> overlays) {
  return td::actor::create_actor<validator::ValidatorManagerImpl>("manager", std::move(opts), db_root, keyring, adnl,
                                                                  rldp, overlays);
}

void ValidatorManagerImpl::record_collate_query_stats(BlockIdExt block_id, double work_time, double cpu_work_time,
                                                      td::optional<CollationStats> stats) {
  if (!stats) {
    ++(block_id.is_masterchain() ? total_collated_blocks_master_error_ : total_collated_blocks_shard_error_);
    return;
  }
  auto &record = new_block_stats_record(block_id);
  record.collator_work_time_ = work_time;
  record.collator_cpu_work_time_ = cpu_work_time;
  record.collator_stats_ = std::move(stats.value());
  ++(block_id.is_masterchain() ? total_collated_blocks_master_ok_ : total_collated_blocks_shard_ok_);
}

void ValidatorManagerImpl::record_validate_query_stats(BlockIdExt block_id, double work_time, double cpu_work_time,
                                                       bool success) {
  auto &record = new_block_stats_record(block_id);
  record.validator_work_time_ = work_time;
  record.validator_cpu_work_time_ = cpu_work_time;
  if (success) {
    ++(block_id.is_masterchain() ? total_validated_blocks_master_ok_ : total_validated_blocks_shard_ok_);
  } else {
    ++(block_id.is_masterchain() ? total_validated_blocks_master_error_ : total_validated_blocks_shard_error_);
  }
}

ValidatorManagerImpl::RecordedBlockStats &ValidatorManagerImpl::new_block_stats_record(BlockIdExt block_id) {
  if (!recorded_block_stats_.count(block_id)) {
    recorded_block_stats_lru_.push(block_id);
    if (recorded_block_stats_lru_.size() > 4096) {
      recorded_block_stats_.erase(recorded_block_stats_lru_.front());
      recorded_block_stats_lru_.pop();
    }
  }
  return recorded_block_stats_[block_id];
}

void ValidatorManagerImpl::register_stats_provider(
    td::uint64 idx, std::string prefix,
    std::function<void(td::Promise<std::vector<std::pair<std::string, std::string>>>)> callback) {
  stats_providers_[idx] = {std::move(prefix), std::move(callback)};
}

void ValidatorManagerImpl::unregister_stats_provider(td::uint64 idx) {
  stats_providers_.erase(idx);
}

size_t ValidatorManagerImpl::CheckedExtMsgCounter::get_msg_count(WorkchainId wc, StdSmcAddress addr) {
  before_query();
  auto it1 = counter_cur_.find({wc, addr});
  auto it2 = counter_prev_.find({wc, addr});
  return (it1 == counter_cur_.end() ? 0 : it1->second) + (it2 == counter_prev_.end() ? 0 : it2->second);
}
size_t ValidatorManagerImpl::CheckedExtMsgCounter::inc_msg_count(WorkchainId wc, StdSmcAddress addr) {
  before_query();
  auto it2 = counter_prev_.find({wc, addr});
  return (it2 == counter_prev_.end() ? 0 : it2->second) + ++counter_cur_[{wc, addr}];
}
void ValidatorManagerImpl::CheckedExtMsgCounter::before_query() {
  while (cleanup_at_.is_in_past()) {
    counter_prev_ = std::move(counter_cur_);
    counter_cur_.clear();
    if (counter_prev_.empty()) {
      cleanup_at_ = td::Timestamp::in(max_ext_msg_per_addr_time_window() / 2.0);
      break;
    }
    cleanup_at_ += max_ext_msg_per_addr_time_window() / 2.0;
  }
}

void ValidatorManagerImpl::init_validator_telemetry() {
  if (last_masterchain_state_.is_null()) {
    return;
  }
  td::Ref<ValidatorSet> validator_set = last_masterchain_state_->get_total_validator_set(0);
  if (validator_set.is_null()) {
    validator_telemetry_.clear();
    return;
  }
  std::set<PublicKeyHash> processed;
  for (auto& key : temp_keys_) {
    if (const ValidatorDescr* desc = validator_set->get_validator(key.bits256_value())) {
      processed.insert(key);
      adnl::AdnlNodeIdShort adnl_id;
      if (desc->addr.is_zero()) {
        adnl_id = adnl::AdnlNodeIdShort{ValidatorFullId{desc->key}.compute_short_id()};
      } else {
        adnl_id = adnl::AdnlNodeIdShort{desc->addr};
      }
      auto& telemetry = validator_telemetry_[key];
      if (telemetry.empty()) {
        telemetry = td::actor::create_actor<ValidatorTelemetry>(
            "telemetry", key, adnl_id, opts_->zero_block_id().file_hash, actor_id(this));
      }
    }
  }
  for (auto it = validator_telemetry_.begin(); it != validator_telemetry_.end();) {
    if (processed.contains(it->first)) {
      ++it;
    } else {
      it = validator_telemetry_.erase(it);
    }
  }
}

}  // namespace validator

}  // namespace ton
