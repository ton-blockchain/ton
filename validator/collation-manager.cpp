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
#include "collation-manager.hpp"

#include "collator-node.hpp"
#include "fabric.h"
#include "td/utils/Random.h"

#include <delay.h>
#include <openssl/lhash.h>
#include <ton/ton-tl.hpp>

namespace ton::validator {

void CollationManager::start_up() {
  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, local_id_);
  update_collators_list(*opts_->get_collators_list());
}

void CollationManager::collate_block(ShardIdFull shard, BlockIdExt min_masterchain_block_id,
                                     std::vector<BlockIdExt> prev, Ed25519_PublicKey creator,
                                     td::Ref<ValidatorSet> validator_set, td::uint64 max_answer_size,
                                     td::CancellationToken cancellation_token, td::Promise<BlockCandidate> promise) {
  if (shard.is_masterchain()) {
    run_collate_query(shard, min_masterchain_block_id, std::move(prev), creator, std::move(validator_set),
                      opts_->get_collator_options(), manager_, td::Timestamp::in(10.0), std::move(promise),
                      std::move(cancellation_token), 0);
    return;
  }
  collate_shard_block(shard, min_masterchain_block_id, std::move(prev), creator, std::move(validator_set),
                      max_answer_size, std::move(cancellation_token), std::move(promise), td::Timestamp::in(10.0));
}

void CollationManager::collate_shard_block(ShardIdFull shard, BlockIdExt min_masterchain_block_id,
                                           std::vector<BlockIdExt> prev, Ed25519_PublicKey creator,
                                           td::Ref<ValidatorSet> validator_set, td::uint64 max_answer_size,
                                           td::CancellationToken cancellation_token,
                                           td::Promise<BlockCandidate> promise, td::Timestamp timeout) {
  TRY_STATUS_PROMISE(promise, cancellation_token.check());
  ShardInfo* s = select_shard_info(shard);
  if (s == nullptr) {
    promise.set_error(
        td::Status::Error(PSTRING() << "shard " << shard.to_str() << " is not configured in collators list"));
    return;
  }

  adnl::AdnlNodeIdShort selected_collator = adnl::AdnlNodeIdShort::zero();
  size_t selected_idx = 0;
  switch (s->select_mode) {
    case CollatorsList::mode_random: {
      int cnt = 0;
      for (size_t i = 0; i < s->collators.size(); ++i) {
        adnl::AdnlNodeIdShort collator = s->collators[i];
        if (collators_[collator].alive) {
          ++cnt;
          if (td::Random::fast(1, cnt) == 1) {
            selected_collator = collator;
            selected_idx = i;
          }
        }
      }
      break;
    }
    case CollatorsList::mode_ordered: {
      for (size_t i = 0; i < s->collators.size(); ++i) {
        adnl::AdnlNodeIdShort collator = s->collators[i];
        if (collators_[collator].alive) {
          selected_collator = collator;
          selected_idx = i;
          break;
        }
      }
      break;
    }
    case CollatorsList::mode_round_robin: {
      size_t iters = 0;
      for (size_t i = s->cur_idx; iters < s->collators.size(); (++i) %= s->collators.size()) {
        adnl::AdnlNodeIdShort& collator = s->collators[i];
        if (collators_[collator].alive) {
          selected_collator = collator;
          selected_idx = i;
          s->cur_idx = (i + 1) % s->collators.size();
          break;
        }
      }
      break;
    }
  }

  if (selected_collator.is_zero() && s->self_collate) {
    run_collate_query(shard, min_masterchain_block_id, std::move(prev), creator, std::move(validator_set),
                      opts_->get_collator_options(), manager_, td::Timestamp::in(10.0), std::move(promise),
                      std::move(cancellation_token), 0);
    return;
  }

  std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>> prev_blocks;
  BlockId next_block_id{shard, 0};
  for (const BlockIdExt& p : prev) {
    prev_blocks.push_back(create_tl_block_id(p));
    next_block_id.seqno = std::max(next_block_id.seqno, p.seqno() + 1);
  }

  promise = [=, SelfId = actor_id(this), promise = std::move(promise),
             retry_at = td::Timestamp::in(0.5)](td::Result<BlockCandidate> R) mutable {
    if (R.is_ok()) {
      promise.set_value(R.move_as_ok());
      return;
    }
    if (!selected_collator.is_zero()) {
      td::actor::send_closure(SelfId, &CollationManager::on_collate_query_error, selected_collator);
    }
    LOG(INFO) << "ERROR: collate query for " << next_block_id.to_str() << " to #" << selected_idx << " ("
              << selected_collator << "): " << R.error();
    if (timeout < retry_at) {
      promise.set_error(R.move_as_error());
      return;
    }
    delay_action(
        [=, promise = std::move(promise)]() mutable {
          td::actor::send_closure(SelfId, &CollationManager::collate_shard_block, shard, min_masterchain_block_id, prev,
                                  creator, validator_set, max_answer_size, cancellation_token, std::move(promise),
                                  timeout);
        },
        retry_at);
  };

  if (selected_collator.is_zero()) {
    promise.set_error(td::Status::Error(PSTRING() << "shard " << shard.to_str() << " has no alive collator node"));
    return;
  }

  td::BufferSlice query = create_serialize_tl_object<ton_api::collatorNode_generateBlock>(
      create_tl_shard_id(shard), validator_set->get_catchain_seqno(), std::move(prev_blocks), creator.as_bits256());
  LOG(INFO) << "sending collate query for " << next_block_id.to_str() << ": send to #" << selected_idx << "("
            << selected_collator << ")";

  td::Promise<td::BufferSlice> P = [=, SelfId = actor_id(this), promise = std::move(promise),
                                    timer = td::Timer()](td::Result<td::BufferSlice> R) mutable {
    TRY_RESULT_PROMISE_PREFIX(promise, data, std::move(R), "rldp query failed: ");
    auto r_error = fetch_tl_object<ton_api::collatorNode_error>(data, true);
    if (r_error.is_ok()) {
      auto error = r_error.move_as_ok();
      promise.set_error(td::Status::Error(error->code_, error->message_));
      return;
    }
    TRY_RESULT_PROMISE(promise, f, fetch_tl_object<ton_api::collatorNode_Candidate>(data, true));
    TRY_RESULT_PROMISE(promise, candidate,
                       CollatorNode::deserialize_candidate(std::move(f), td::narrow_cast<int>(max_answer_size)));
    if (candidate.pubkey.as_bits256() != creator.as_bits256()) {
      promise.set_error(td::Status::Error("collate query: block candidate source mismatch"));
      return;
    }
    if (candidate.id.id != next_block_id) {
      promise.set_error(td::Status::Error("collate query: block id mismatch"));
      return;
    }
    LOG(INFO) << "got collated block " << next_block_id.to_str() << " from #" << selected_idx << " ("
              << selected_collator << ") in " << timer.elapsed() << "s";
    promise.set_result(std::move(candidate));
  };
  td::actor::send_closure(rldp_, &rldp::Rldp::send_query_ex, local_id_, selected_collator, "collatequery", std::move(P),
                          timeout, std::move(query), max_answer_size);
}

void CollationManager::update_options(td::Ref<ValidatorManagerOptions> opts) {
  auto old_list = opts_->get_collators_list();
  opts_ = std::move(opts);
  auto list = opts_->get_collators_list();
  if (old_list != list) {
    update_collators_list(*list);
  }
}

void CollationManager::validator_group_started(ShardIdFull shard) {
  if (active_validator_groups_[shard]++ != 0) {
    return;
  }
  ShardInfo* s = select_shard_info(shard);
  if (s == nullptr) {
    return;
  }
  if (s->active_cnt++ != 0) {
    return;
  }
  for (adnl::AdnlNodeIdShort id : s->collators) {
    CollatorInfo& collator = collators_[id];
    collator.active_cnt++;
  }
  alarm();
}

void CollationManager::validator_group_finished(ShardIdFull shard) {
  if (--active_validator_groups_[shard] != 0) {
    return;
  }
  active_validator_groups_.erase(shard);
  ShardInfo* s = select_shard_info(shard);
  if (s == nullptr) {
    return;
  }
  if (--s->active_cnt != 0) {
    return;
  }
  for (adnl::AdnlNodeIdShort id : s->collators) {
    CollatorInfo& collator = collators_[id];
    --collator.active_cnt;
  }
  alarm();
}

void CollationManager::get_stats(
    td::Promise<tl_object_ptr<ton_api::engine_validator_collationManagerStats_localId>> promise) {
  auto stats = create_tl_object<ton_api::engine_validator_collationManagerStats_localId>();
  stats->adnl_id_ = local_id_.bits256_value();
  for (ShardInfo& s : shards_) {
    auto obj = create_tl_object<ton_api::engine_validator_collationManagerStats_shard>();
    obj->shard_id_ = create_tl_shard_id(s.shard_id);
    obj->active_ = s.active_cnt;
    obj->self_collate_ = s.self_collate;
    switch (s.select_mode) {
      case CollatorsList::mode_random:
        obj->select_mode_ = "random";
        break;
      case CollatorsList::mode_ordered:
        obj->select_mode_ = "ordered";
        break;
      case CollatorsList::mode_round_robin:
        obj->select_mode_ = "round_robin";
        break;
    }
    for (adnl::AdnlNodeIdShort& id : s.collators) {
      obj->collators_.push_back(id.bits256_value());
    }
    stats->shards_.push_back(std::move(obj));
  }
  for (auto& [id, collator] : collators_) {
    auto obj = create_tl_object<ton_api::engine_validator_collationManagerStats_collator>();
    obj->adnl_id_ = id.bits256_value();
    obj->active_ = collator.active_cnt;
    obj->alive_ = collator.alive;
    if (collator.active_cnt && !collator.sent_ping) {
      obj->ping_in_ = collator.ping_at.in();
    } else {
      obj->ping_in_ = -1.0;
    }
    stats->collators_.push_back(std::move(obj));
  }
  promise.set_value(std::move(stats));
}

void CollationManager::update_collators_list(const CollatorsList& collators_list) {
  shards_.clear();
  for (auto& [_, collator] : collators_) {
    collator.active_cnt = 0;
  }
  auto old_collators = std::move(collators_);
  collators_.clear();
  for (const auto& shard : collators_list.shards) {
    shards_.push_back({.shard_id = shard.shard_id, .select_mode = shard.select_mode, .collators = shard.collators});
    for (auto id : shard.collators) {
      auto it = old_collators.find(id);
      if (it == old_collators.end()) {
        collators_[id];
      } else {
        collators_[id] = std::move(it->second);
        old_collators.erase(it);
      }
    }
  }
  for (auto& [shard, _] : active_validator_groups_) {
    ShardInfo* s = select_shard_info(shard);
    if (s == nullptr) {
      continue;
    }
    if (s->active_cnt++ != 0) {
      continue;
    }
    for (adnl::AdnlNodeIdShort id : s->collators) {
      CollatorInfo& collator = collators_[id];
      collator.active_cnt++;
    }
  }
  alarm();
}

CollationManager::ShardInfo* CollationManager::select_shard_info(ShardIdFull shard) {
  for (auto& s : shards_) {
    if (shard_intersects(shard, s.shard_id)) {
      return &s;
    }
  }
  return nullptr;
}

void CollationManager::alarm() {
  alarm_timestamp() = td::Timestamp::never();
  for (auto& [id, collator] : collators_) {
    if (collator.active_cnt == 0 || collator.sent_ping) {
      continue;
    }
    if (collator.ping_at.is_in_past()) {
      collator.sent_ping = true;
      td::BufferSlice query = create_serialize_tl_object<ton_api::collatorNode_ping>(0);
      td::Promise<td::BufferSlice> P = [=, SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable {
        td::actor::send_closure(SelfId, &CollationManager::got_pong, id, std::move(R));
      };
      LOG(DEBUG) << "sending ping to " << id;
      td::actor::send_closure(rldp_, &rldp::Rldp::send_query, local_id_, id, "collatorping", std::move(P),
                              td::Timestamp::in(2.0), std::move(query));
    } else {
      alarm_timestamp().relax(collator.ping_at);
    }
  }
}

void CollationManager::got_pong(adnl::AdnlNodeIdShort id, td::Result<td::BufferSlice> R) {
  auto it = collators_.find(id);
  if (it == collators_.end()) {
    return;
  }
  CollatorInfo& collator = it->second;
  collator.sent_ping = false;

  auto r_pong = [&]() -> td::Result<tl_object_ptr<ton_api::collatorNode_pong>> {
    TRY_RESULT_PREFIX(data, std::move(R), "rldp query error: ");
    auto r_error = fetch_tl_object<ton_api::collatorNode_error>(data, true);
    if (r_error.is_ok()) {
      auto error = r_error.move_as_ok();
      return td::Status::Error(error->code_, error->message_);
    }
    return fetch_tl_object<ton_api::collatorNode_pong>(data, true);
  }();
  if (r_pong.is_error()) {
    LOG(DEBUG) << "pong from " << id << " : " << r_pong.move_as_error();
    collator.alive = false;
  } else {
    LOG(DEBUG) << "pong from " << id << " : OK";
    collator.alive = true;
  }
  collator.ping_at = td::Timestamp::in(td::Random::fast(10.0, 20.0));
  if (collator.active_cnt && !collator.sent_ping) {
    alarm_timestamp().relax(collator.ping_at);
  }
}

void CollationManager::on_collate_query_error(adnl::AdnlNodeIdShort id) {
  auto it = collators_.find(id);
  if (it == collators_.end()) {
    return;
  }
  CollatorInfo& collator = it->second;
  collator.ping_at = td::Timestamp::now();
  if (collator.active_cnt && !collator.sent_ping) {
    alarm_timestamp().relax(collator.ping_at);
  }
}

}  // namespace ton::validator
