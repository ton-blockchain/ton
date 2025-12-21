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
*/

#include "collator-node-session.hpp"
#include "collator-node.hpp"
#include "fabric.h"
#include "utils.hpp"

namespace ton::validator {

static BlockSeqno get_next_block_seqno(const std::vector<BlockIdExt>& prev) {
  if (prev.size() == 1) {
    return prev[0].seqno() + 1;
  }
  CHECK(prev.size() == 2);
  return std::max(prev[0].seqno(), prev[1].seqno()) + 1;
}

CollatorNodeSession::CollatorNodeSession(ShardIdFull shard, std::vector<BlockIdExt> prev,
                                         td::Ref<ValidatorSet> validator_set, BlockIdExt min_masterchain_block_id,
                                         bool can_generate, Ref<MasterchainState> state, adnl::AdnlNodeIdShort local_id,
                                         td::Ref<ValidatorManagerOptions> opts,
                                         td::actor::ActorId<ValidatorManager> manager,
                                         td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp2::Rldp> rldp)
    : shard_(shard)
    , prev_(std::move(prev))
    , validator_set_(validator_set)
    , min_masterchain_block_id_(min_masterchain_block_id)
    , can_generate_(can_generate)
    , local_id_(local_id)
    , opts_(opts)
    , manager_(manager)
    , adnl_(adnl)
    , rldp_(rldp)
    , next_block_seqno_(get_next_block_seqno(prev_)) {
  update_masterchain_config(state);
}

void CollatorNodeSession::start_up() {
  LOG(INFO) << "Starting collator node session, shard " << shard_.to_str() << ", cc_seqno "
            << validator_set_->get_catchain_seqno() << ", next block seqno " << next_block_seqno_;

  if (can_generate_) {
    generate_block(prev_, {}, {}, td::Timestamp::in(10.0), [](td::Result<BlockCandidate>) {});
  }
}

void CollatorNodeSession::tear_down() {
  LOG(INFO) << "Finishing collator node session, shard " << shard_.to_str() << ", cc_seqno "
            << validator_set_->get_catchain_seqno();
  for (auto& [_, entry] : cache_) {
    entry->cancel(td::Status::Error("validator session finished"));
  }
}

void CollatorNodeSession::new_shard_block_accepted(BlockIdExt block_id, bool can_generate) {
  CHECK(block_id.shard_full() == shard_);
  can_generate_ = can_generate;
  if (next_block_seqno_ > block_id.seqno()) {
    return;
  }
  LOG(DEBUG) << "New shard block " << block_id.to_str();
  next_block_seqno_ = block_id.seqno() + 1;
  prev_ = {block_id};

  while (!cache_.empty()) {
    auto& [cache_prev, entry] = *cache_.begin();
    if (entry->block_seqno < next_block_seqno_) {
      entry->cancel(td::Status::Error(PSTRING() << "next block seqno " << entry->block_seqno << " is too old, expected "
                                                << next_block_seqno_));
    } else if (entry->block_seqno == next_block_seqno_ && prev_ != cache_prev) {
      entry->cancel(td::Status::Error(PSTRING() << "invalid prev blocks for seqno " << entry->block_seqno));
    } else {
      break;
    }
    if (!entry->has_external_query_at && entry->has_internal_query_at) {
      LOG(INFO) << "generate block query"
                << ": shard=" << shard_.to_str() << ", cc_seqno=" << validator_set_->get_catchain_seqno()
                << ", next_block_seqno=" << entry->block_seqno
                << ": nobody asked for block, but we tried to generate it";
    }
    if (entry->has_external_query_at && !entry->has_internal_query_at) {
      LOG(INFO) << "generate block query"
                << ": shard=" << shard_.to_str() << ", cc_seqno=" << validator_set_->get_catchain_seqno()
                << ", next_block_seqno=" << entry->block_seqno
                << ": somebody asked for block we didn't even try to generate";
    }
    cache_.erase(cache_.begin());
  }

  if (can_generate_) {
    generate_block(prev_, {}, {}, td::Timestamp::in(10.0), [](td::Result<BlockCandidate>) {});
  }
}

void CollatorNodeSession::update_masterchain_config(td::Ref<MasterchainState> state) {
  ValidatorSessionConfig config = state->get_consensus_config();
  proto_version_ = config.proto_version;
  max_candidate_size_ = config.max_block_size + config.max_collated_data_size + 1024;
}

void CollatorNodeSession::generate_block(std::vector<BlockIdExt> prev_blocks,
                                         td::optional<BlockCandidatePriority> o_priority,
                                         td::Ref<BlockData> o_optimistic_prev_block, td::Timestamp timeout,
                                         td::Promise<BlockCandidate> promise) {
  bool is_external = !o_priority;
  bool is_optimistic = o_optimistic_prev_block.not_null();
  BlockSeqno block_seqno = get_next_block_seqno(prev_blocks);
  if (next_block_seqno_ > block_seqno) {
    promise.set_error(td::Status::Error(PSTRING() << "next block seqno " << block_seqno << " is too old, expected "
                                                  << next_block_seqno_));
    return;
  }
  if (next_block_seqno_ == block_seqno && prev_ != prev_blocks) {
    promise.set_error(td::Status::Error("invalid prev_blocks"));
    return;
  }
  if (next_block_seqno_ + 10 < block_seqno) {
    promise.set_error(td::Status::Error(PSTRING() << "next block seqno " << block_seqno << " is too new, current is "
                                                  << next_block_seqno_));
    return;
  }

  static auto prefix_inner = [](td::StringBuilder& sb, const ShardIdFull& shard, CatchainSeqno cc_seqno,
                                BlockSeqno block_seqno, const td::optional<BlockCandidatePriority>& o_priority,
                                bool is_optimistic) {
    sb << "generate block query"
       << ": shard=" << shard.to_str() << ", cc_seqno=" << cc_seqno << ", next_block_seqno=" << block_seqno;
    if (o_priority) {
      sb << " external{";
      sb << "round_offset=" << o_priority.value().round - o_priority.value().first_block_round
         << ",priority=" << o_priority.value().priority;
      sb << ",first_block_round=" << o_priority.value().first_block_round;
      sb << "}";
    } else {
      sb << " internal";
    }
    if (is_optimistic) {
      sb << " opt";
    }
  };
  auto prefix = [&](td::StringBuilder& sb) {
    prefix_inner(sb, shard_, validator_set_->get_catchain_seqno(), block_seqno, o_priority, is_optimistic);
  };

  auto cache_entry = cache_[prev_blocks];
  if (cache_entry == nullptr) {
    cache_entry = cache_[prev_blocks] = std::make_shared<CacheEntry>();
  }
  if (is_external && !cache_entry->has_external_query_at) {
    cache_entry->has_external_query_at = td::Timestamp::now();
    if (cache_entry->has_internal_query_at && cache_entry->has_external_query_at) {
      FLOG(INFO) {
        prefix(sb);
        sb << ": got external query " << cache_entry->has_external_query_at - cache_entry->has_internal_query_at
           << "s  after internal query [WON]";
      };
    }
  }
  if (!is_external && !cache_entry->has_internal_query_at) {
    cache_entry->has_internal_query_at = td::Timestamp::now();
    if (cache_entry->has_internal_query_at && cache_entry->has_external_query_at) {
      FLOG(INFO) {
        prefix(sb);
        sb << ": got internal query " << cache_entry->has_internal_query_at - cache_entry->has_external_query_at
           << "s after external query [LOST]";
      };
    }
  }
  if (cache_entry->result) {
    auto has_result_ago = td::Timestamp::now() - cache_entry->has_result_at;
    FLOG(INFO) {
      prefix(sb);
      sb << ": using cached result " << " generated " << has_result_ago << "s ago";
      sb << (is_external ? " for external query [WON]" : " for internal query ");
    };
    promise.set_result(cache_entry->result.value().clone());
    return;
  }
  cache_entry->promises.push_back(std::move(promise));

  if (cache_entry->started) {
    FLOG(INFO) {
      prefix(sb);
      sb << ": collation in progress, waiting";
    };
    return;
  }
  FLOG(INFO) {
    prefix(sb);
    sb << ": starting collation";
  };
  cache_entry->started = true;
  cache_entry->block_seqno = block_seqno;
  run_collate_query(CollateParams{.shard = shard_,
                                  .min_masterchain_block_id = min_masterchain_block_id_,
                                  .prev = std::move(prev_blocks),
                                  .validator_set = validator_set_,
                                  .collator_opts = opts_->get_collator_options(),
                                  .collator_node_id = local_id_,
                                  .skip_store_candidate = true,
                                  .optimistic_prev_block = o_optimistic_prev_block},
                    manager_, timeout, cache_entry->cancellation_token_source.get_cancellation_token(),
                    [=, shard = shard_, cc_seqno = validator_set_->get_catchain_seqno(), SelfId = actor_id(this),
                     timer = td::Timer{}](td::Result<BlockCandidate> R) mutable {
                      FLOG(INFO) {
                        prefix_inner(sb, shard, cc_seqno, block_seqno, o_priority, is_optimistic);
                        sb << ": " << (R.is_ok() ? "OK" : R.error().to_string()) << " time=" << timer.elapsed();
                      };
                      td::actor::send_closure(SelfId, &CollatorNodeSession::process_result, cache_entry, std::move(R));
                    });
}

void CollatorNodeSession::process_result(std::shared_ptr<CacheEntry> cache_entry, td::Result<BlockCandidate> R) {
  if (R.is_error()) {
    cache_entry->started = false;
    for (auto& p : cache_entry->promises) {
      p.set_error(R.error().clone());
    }
  } else {
    cache_entry->result = R.move_as_ok();
    cache_entry->has_result_at = td::Timestamp::now();
    for (auto& p : cache_entry->promises) {
      p.set_result(cache_entry->result.value().clone());
    }
  }
  cache_entry->promises.clear();
}

void CollatorNodeSession::process_request(adnl::AdnlNodeIdShort src, std::vector<BlockIdExt> prev_blocks,
                                          BlockCandidatePriority priority, bool is_optimistic, td::Timestamp timeout,
                                          td::Promise<BlockCandidate> promise) {
  if (is_optimistic) {
    if (prev_blocks.size() != 1) {
      promise.set_error(td::Status::Error("optimistic collation, expected 1 prev block"));
      return;
    }
    auto it = cache_.find(prev_blocks);
    if (it == cache_.end() || it->second->started) {
      BlockIdExt prev_block = prev_blocks[0];
      td::actor::send_closure(
          manager_, &ValidatorManager::get_candidate_data_by_block_id_from_db, prev_block,
          [=, SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
            td::actor::send_closure(SelfId, &CollatorNodeSession::process_request_optimistic_cont, src, prev_block,
                                    priority, timeout, std::move(promise), std::move(R));
          });
      return;
    }
  }
  generate_block(std::move(prev_blocks), priority, {}, timeout, std::move(promise));
}

void CollatorNodeSession::process_request_optimistic_cont(adnl::AdnlNodeIdShort src, BlockIdExt prev_block_id,
                                                          BlockCandidatePriority priority, td::Timestamp timeout,
                                                          td::Promise<BlockCandidate> promise,
                                                          td::Result<td::BufferSlice> prev_block_data) {
  if (prev_block_data.is_ok()) {
    TRY_RESULT_PROMISE_PREFIX(promise, prev_block, create_block(prev_block_id, prev_block_data.move_as_ok()),
                              "invalid prev block data in db: ");
    LOG(INFO) << "got prev block from db for optimistic collation: " << prev_block_id.to_str();
    generate_block({prev_block_id}, priority, prev_block, timeout, std::move(promise));
    return;
  }
  td::actor::send_closure(
      rldp_, &rldp2::Rldp::send_query_ex, local_id_, src, "getprevblock",
      [=, SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
        td::actor::send_closure(SelfId, &CollatorNodeSession::process_request_optimistic_cont2, prev_block_id, priority,
                                timeout, std::move(promise), std::move(R));
      },
      timeout,
      create_serialize_tl_object<ton_api::collatorNode_requestBlockCallback>(0, create_tl_block_id(prev_block_id)),
      max_candidate_size_);
}

void CollatorNodeSession::process_request_optimistic_cont2(BlockIdExt prev_block_id, BlockCandidatePriority priority,
                                                           td::Timestamp timeout, td::Promise<BlockCandidate> promise,
                                                           td::Result<td::BufferSlice> R) {
  TRY_RESULT_PROMISE_PREFIX(promise, response, std::move(R),
                            "failed to download prev block data for optimistic collation: ");
  TRY_RESULT_PROMISE_PREFIX(promise, f, fetch_tl_object<ton_api::collatorNode_Candidate>(response, true),
                            "failed to download prev block data for optimistic collation: ");
  TRY_RESULT_PROMISE_PREFIX(promise, candidate,
                            deserialize_candidate(std::move(f), max_candidate_size_, proto_version_),
                            "failed to download prev block data for optimistic collation: ");
  TRY_RESULT_PROMISE_PREFIX(promise, prev_block, create_block(prev_block_id, std::move(candidate.data)),
                            "invalid prev block data from validator: ");
  LOG(INFO) << "got prev block from validator for optimistic collation: " << prev_block_id.to_str();
  generate_block({prev_block_id}, priority, prev_block, timeout, std::move(promise));
}

void CollatorNodeSession::CacheEntry::cancel(td::Status reason) {
  for (auto& promise : promises) {
    promise.set_error(reason.clone());
  }
  promises.clear();
  cancellation_token_source.cancel();
}

}  // namespace ton::validator
