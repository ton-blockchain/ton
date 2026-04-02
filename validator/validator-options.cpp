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
#include <ton/ton-tl.hpp>

#include "ton/ton-shard.h"

#include "validator-options.hpp"

namespace ton {

namespace validator {

td::Status CollatorsList::unpack(const ton_api::engine_validator_collatorsList& obj) {
  shards.clear();
  self_collate = false;
  for (const auto& shard_obj : obj.shards_) {
    ShardIdFull shard_id = create_shard_id(shard_obj->shard_id_);
    if (shard_id.is_masterchain()) {
      return td::Status::Error("masterchain shard in collators list");
    }
    if (!shard_id.is_valid_ext()) {
      return td::Status::Error(PSTRING() << "invalid shard " << shard_id.to_str());
    }
    shards.emplace_back();
    Shard& shard = shards.back();
    shard.shard_id = shard_id;
    shard.self_collate = shard_obj->self_collate_;
    if (shard.self_collate) {
      self_collate = true;
    }
    if (shard_obj->select_mode_.empty() || shard_obj->select_mode_ == "random") {
      shard.select_mode = mode_random;
    } else if (shard_obj->select_mode_ == "ordered") {
      shard.select_mode = mode_ordered;
    } else if (shard_obj->select_mode_ == "round_robin") {
      shard.select_mode = mode_round_robin;
    } else {
      return td::Status::Error(PSTRING() << "invalid select mode '" << shard_obj->select_mode_
                                         << "' (allowed: 'random', 'ordered', 'round_robin')");
    }
    for (const auto& collator : shard_obj->collators_) {
      shard.collators.push_back(adnl::AdnlNodeIdShort{collator->adnl_id_});
    }
  }
  return td::Status::OK();
}

CollatorsList CollatorsList::default_list() {
  CollatorsList list;
  list.shards.push_back({
      .shard_id = ShardIdFull{basechainId, shardIdAll},
      .select_mode = mode_random,
      .collators = {},
      .self_collate = true,
  });
  list.self_collate = true;
  return list;
}

td::Status ShardBlockVerifierConfig::unpack(const ton_api::engine_validator_shardBlockVerifierConfig& obj) {
  shards.clear();
  for (const auto& shard_obj : obj.shards_) {
    Shard shard;
    shard.shard_id = create_shard_id(shard_obj->shard_id_);
    if (shard.shard_id.is_masterchain() || !shard.shard_id.is_valid_ext()) {
      return td::Status::Error(PSTRING() << "invalid shard " << shard.shard_id.to_str());
    }
    std::set<adnl::AdnlNodeIdShort> trusted_nodes;
    for (const td::Bits256& id : shard_obj->trusted_nodes_) {
      adnl::AdnlNodeIdShort node_id{id};
      if (!trusted_nodes.insert(node_id).second) {
        return td::Status::Error(PSTRING() << "duplicate node " << node_id);
      }
      shard.trusted_nodes.push_back(node_id);
    }
    if (shard_obj->required_confirms_ < 0 || shard_obj->required_confirms_ > (int)shard.trusted_nodes.size()) {
      return td::Status::Error(PSTRING() << "invalid required_confirms " << shard_obj->required_confirms_
                                         << " for shard " << shard.shard_id.to_str()
                                         << " (nodes: " << shard.trusted_nodes.size() << ")");
    }
    shard.required_confirms = shard_obj->required_confirms_;
    shards.push_back(std::move(shard));
  }
  return td::Status::OK();
}

td::Status NoncriticalParamsOverride::validate_user_input(const Params& params) {
  auto validate_duration_range = [](td::Slice name, const auto& value, td::int64 min_value_ms,
                                    td::optional<td::int64> max_value_ms = {}) -> td::Status {
    auto ms = value.count();
    if (ms < min_value_ms) {
      return td::Status::Error(PSTRING() << name << " must be at least " << min_value_ms << "ms, got " << ms
                                         << "ms");
    }
    if (max_value_ms && ms > *max_value_ms) {
      return td::Status::Error(PSTRING() << name << " must not exceed " << *max_value_ms << "ms, got " << ms
                                         << "ms");
    }
    return td::Status::OK();
  };

  if (params.target_rate.has_value()) {
    TRY_STATUS(validate_duration_range("target_rate_ms", *params.target_rate, MIN_TARGET_RATE_MS, MAX_TARGET_RATE_MS));
  }
  if (params.first_block_timeout.has_value()) {
    TRY_STATUS(validate_duration_range("first_block_timeout_ms", *params.first_block_timeout, 1));
  }
  if (params.first_block_timeout_multiplier.has_value() && *params.first_block_timeout_multiplier <= 1.0) {
    return td::Status::Error(PSTRING() << "first_block_timeout_multiplier must be greater than 1, got "
                                       << *params.first_block_timeout_multiplier);
  }
  if (params.first_block_timeout_cap.has_value()) {
    TRY_STATUS(validate_duration_range("first_block_timeout_cap_ms", *params.first_block_timeout_cap, 1,
                                       MAX_FIRST_BLOCK_TIMEOUT_CAP_MS));
  }
  if (params.candidate_resolve_timeout.has_value()) {
    TRY_STATUS(validate_duration_range("candidate_resolve_timeout_ms", *params.candidate_resolve_timeout, 1));
  }
  if (params.candidate_resolve_timeout_multiplier.has_value() && *params.candidate_resolve_timeout_multiplier <= 1.0) {
    return td::Status::Error(PSTRING() << "candidate_resolve_timeout_multiplier must be greater than 1, got "
                                       << *params.candidate_resolve_timeout_multiplier);
  }
  if (params.candidate_resolve_timeout_cap.has_value()) {
    TRY_STATUS(validate_duration_range("candidate_resolve_timeout_cap_ms", *params.candidate_resolve_timeout_cap, 1));
  }
  if (params.candidate_resolve_cooldown.has_value()) {
    TRY_STATUS(validate_duration_range("candidate_resolve_cooldown_ms", *params.candidate_resolve_cooldown, 1));
  }
  if (params.standstill_timeout.has_value()) {
    TRY_STATUS(validate_duration_range("standstill_timeout_ms", *params.standstill_timeout, 1));
  }
  if (params.standstill_max_egress_bytes_per_s.has_value() && *params.standstill_max_egress_bytes_per_s == 0) {
    return td::Status::Error("standstill_max_egress_bytes_per_s must be positive");
  }
  if (params.bad_signature_ban_duration.has_value()) {
    TRY_STATUS(validate_duration_range("bad_signature_ban_duration_ms", *params.bad_signature_ban_duration, 1));
  }
  if (params.candidate_resolve_rate_limit.has_value() && *params.candidate_resolve_rate_limit == 0) {
    return td::Status::Error("candidate_resolve_rate_limit must be positive");
  }

  if (params.first_block_timeout.has_value() && params.first_block_timeout_cap.has_value() &&
      *params.first_block_timeout_cap < *params.first_block_timeout) {
    return td::Status::Error(PSTRING() << "first_block_timeout_cap_ms must be at least first_block_timeout_ms ("
                                       << params.first_block_timeout->count() << "), got "
                                       << params.first_block_timeout_cap->count());
  }
  if (params.candidate_resolve_timeout.has_value() && params.candidate_resolve_timeout_cap.has_value() &&
      *params.candidate_resolve_timeout_cap < *params.candidate_resolve_timeout) {
    return td::Status::Error(PSTRING() << "candidate_resolve_timeout_cap_ms must be at least "
                                          "candidate_resolve_timeout_ms ("
                                       << params.candidate_resolve_timeout->count() << "), got "
                                       << params.candidate_resolve_timeout_cap->count());
  }
  if (params.first_block_timeout.has_value() && params.candidate_resolve_timeout.has_value() &&
      *params.first_block_timeout < *params.candidate_resolve_timeout) {
    return td::Status::Error(PSTRING() << "first_block_timeout_ms must be at least "
                                          "candidate_resolve_timeout_ms ("
                                       << params.candidate_resolve_timeout->count() << "), got "
                                       << params.first_block_timeout->count());
  }
  if (params.standstill_timeout.has_value() && params.candidate_resolve_timeout_cap.has_value() &&
      *params.standstill_timeout < *params.candidate_resolve_timeout_cap) {
    return td::Status::Error(PSTRING() << "standstill_timeout_ms must be at least "
                                          "candidate_resolve_timeout_cap_ms ("
                                       << params.candidate_resolve_timeout_cap->count() << "), got "
                                       << params.standstill_timeout->count());
  }

  return td::Status::OK();
}

td::Status NoncriticalParamsOverride::validate_against_config(const NewConsensusConfig& config) const {
  TRY_STATUS(validate_user_input(params));

  auto effective = apply(config.noncritical_params);

  if (params.target_rate.has_value() || params.first_block_timeout.has_value()) {
    auto max_first_block_timeout_ms =
        static_cast<td::uint64>(effective.target_rate.count()) * config.slots_per_leader_window;
    if (static_cast<td::uint64>(effective.first_block_timeout.count()) > max_first_block_timeout_ms) {
      return td::Status::Error(PSTRING() << "first_block_timeout_ms must not exceed target_rate_ms * "
                                            "slots_per_leader_window ("
                                         << max_first_block_timeout_ms << "), got "
                                         << effective.first_block_timeout.count());
    }
  }

  if ((params.first_block_timeout.has_value() || params.candidate_resolve_timeout.has_value()) &&
      effective.first_block_timeout < effective.candidate_resolve_timeout) {
    return td::Status::Error(PSTRING() << "first_block_timeout_ms must be at least "
                                          "candidate_resolve_timeout_ms ("
                                       << effective.candidate_resolve_timeout.count() << "), got "
                                       << effective.first_block_timeout.count());
  }

  if ((params.first_block_timeout.has_value() || params.first_block_timeout_cap.has_value()) &&
      effective.first_block_timeout_cap < effective.first_block_timeout) {
    return td::Status::Error(PSTRING() << "first_block_timeout_cap_ms must be at least first_block_timeout_ms ("
                                       << effective.first_block_timeout.count() << "), got "
                                       << effective.first_block_timeout_cap.count());
  }

  if ((params.candidate_resolve_timeout.has_value() || params.candidate_resolve_timeout_cap.has_value()) &&
      effective.candidate_resolve_timeout_cap < effective.candidate_resolve_timeout) {
    return td::Status::Error(PSTRING() << "candidate_resolve_timeout_cap_ms must be at least "
                                          "candidate_resolve_timeout_ms ("
                                       << effective.candidate_resolve_timeout.count() << "), got "
                                       << effective.candidate_resolve_timeout_cap.count());
  }

  if ((params.standstill_timeout.has_value() || params.candidate_resolve_timeout_cap.has_value()) &&
      effective.standstill_timeout < effective.candidate_resolve_timeout_cap) {
    return td::Status::Error(PSTRING() << "standstill_timeout_ms must be at least "
                                          "candidate_resolve_timeout_cap_ms ("
                                       << effective.candidate_resolve_timeout_cap.count() << "), got "
                                       << effective.standstill_timeout.count());
  }

  return td::Status::OK();
}

td::Ref<ValidatorManagerOptions> ValidatorManagerOptions::create(BlockIdExt zero_block_id, BlockIdExt init_block_id,
                                                                 bool allow_blockchain_init, double sync_blocks_before,
                                                                 double block_ttl, double state_ttl, double archive_ttl,
                                                                 double key_proof_ttl, size_t max_mempool_num,
                                                                 bool initial_sync_disabled) {
  return td::make_ref<ValidatorManagerOptionsImpl>(zero_block_id, init_block_id, allow_blockchain_init,
                                                   sync_blocks_before, block_ttl, state_ttl, max_mempool_num,
                                                   archive_ttl, key_proof_ttl, initial_sync_disabled);
}

}  // namespace validator

}  // namespace ton
