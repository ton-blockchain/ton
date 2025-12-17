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
#pragma once

#include <ton/ton-tl.hpp>

#include "adnl/adnl-node-id.hpp"
#include "crypto/common/bitstring.h"
#include "td/utils/int_types.h"
#include "ton/ton-types.h"

namespace ton {

namespace validatorsession {

constexpr int VERBOSITY_NAME(VALIDATOR_SESSION_WARNING) = verbosity_WARNING;
constexpr int VERBOSITY_NAME(VALIDATOR_SESSION_NOTICE) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(VALIDATOR_SESSION_INFO) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(VALIDATOR_SESSION_DEBUG) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(VALIDATOR_SESSION_EXTRA_DEBUG) = verbosity_DEBUG + 1;

using ValidatorSessionRootHash = td::Bits256;
using ValidatorSessionFileHash = td::Bits256;
using ValidatorSessionCollatedDataFileHash = td::Bits256;
using ValidatorSessionCandidateId = td::Bits256;

inline ValidatorSessionCandidateId skip_round_candidate_id() {
  return ValidatorSessionCandidateId::zero();
}

struct ValidatorSessionOptions {
  ValidatorSessionOptions() = default;
  explicit ValidatorSessionOptions(const ValidatorSessionConfig &conf);

  CatChainOptions catchain_opts;

  td::uint32 round_candidates = 3;
  double next_candidate_delay = 2.0;
  td::uint32 round_attempt_duration = 16;
  td::uint32 max_round_attempts = 4;

  td::uint32 max_block_size = 4 << 20;
  td::uint32 max_collated_data_size = 4 << 20;

  bool new_catchain_ids = false;

  td::uint32 proto_version = 0;

  td::Bits256 get_hash() const;
};

struct ValidatorSessionNode {
  PublicKey pub_key;
  adnl::AdnlNodeIdShort adnl_id;
  ValidatorWeight weight;
};

struct ValidatorSessionStats {
  enum { status_none = 0, status_received = 1, status_rejected = 2, status_approved = 3 };
  enum { recv_none = 0, recv_collated = 1, recv_broadcast = 2, recv_query = 3, recv_cached = 4, recv_startup = 5 };

  struct Producer {
    int block_status = status_none;
    PublicKeyHash validator_id = PublicKeyHash::zero();
    ValidatorSessionCandidateId candidate_id = ValidatorSessionCandidateId::zero();
    BlockIdExt block_id{workchainIdNotYet, 0, 0, td::Bits256::zero(), td::Bits256::zero()};
    td::Bits256 collated_data_hash = td::Bits256::zero();
    bool is_accepted = false;
    bool is_ours = false;
    double got_block_at = -1.0;
    int got_block_by = 0;
    double got_submit_at = -1.0;
    td::int32 gen_utime = -1;
    std::string comment;

    double collation_time = -1.0, collated_at = -1.0;
    bool collation_cached = false;
    bool self_collated = false;
    td::Bits256 collator_node_id = td::Bits256::zero();

    double validation_time = -1.0, validated_at = -1.0;
    bool validation_cached = false;

    std::vector<bool> approvers, signers;
    ValidatorWeight approved_weight = 0;
    ValidatorWeight signed_weight = 0;
    double approved_33pct_at = -1.0;
    double approved_66pct_at = -1.0;
    double signed_33pct_at = -1.0;
    double signed_66pct_at = -1.0;

    double serialize_time = -1.0;
    double deserialize_time = -1.0;
    td::int32 serialized_size = -1;

    void set_approved_by(td::uint32 id, ValidatorWeight weight, ValidatorWeight total_weight) {
      if (!approvers.at(id)) {
        approvers.at(id) = true;
        approved_weight += weight;
        if (approved_33pct_at <= 0.0 && approved_weight >= total_weight / 3 + 1) {
          approved_33pct_at = td::Clocks::system();
        }
        if (approved_66pct_at <= 0.0 && approved_weight >= (total_weight * 2) / 3 + 1) {
          approved_66pct_at = td::Clocks::system();
        }
      }
    }

    void set_signed_by(td::uint32 id, ValidatorWeight weight, ValidatorWeight total_weight) {
      if (!signers.at(id)) {
        signers.at(id) = true;
        signed_weight += weight;
        if (signed_33pct_at <= 0.0 && signed_weight >= total_weight / 3 + 1) {
          signed_33pct_at = td::Clocks::system();
        }
        if (signed_66pct_at <= 0.0 && signed_weight >= (total_weight * 2) / 3 + 1) {
          signed_66pct_at = td::Clocks::system();
        }
      }
    }

    tl_object_ptr<ton_api::validatorStats_stats_producer> tl() const {
      std::string approvers_str(approvers.size(), '0');
      for (size_t i = 0; i < approvers.size(); ++i) {
        approvers_str[i] = '0' + approvers[i];
      }
      std::string signers_str(signers.size(), '0');
      for (size_t i = 0; i < signers.size(); ++i) {
        signers_str[i] = '0' + signers[i];
      }
      int flags =
          (block_status != status_none || !candidate_id.is_zero()
               ? ton_api::validatorStats_stats_producer::Flags::BLOCK_ID_MASK
               : 0) |
          (collated_at >= 0.0 ? ton_api::validatorStats_stats_producer::Flags::COLLATED_AT_MASK : 0) |
          (!collator_node_id.is_zero() ? ton_api::validatorStats_stats_producer::Flags::COLLATOR_NODE_ID_MASK : 0) |
          (validated_at >= 0.0 ? ton_api::validatorStats_stats_producer::Flags::VALIDATED_AT_MASK : 0) |
          (serialize_time >= 0.0 || deserialize_time >= 0.0 || serialized_size >= 0
               ? ton_api::validatorStats_stats_producer::Flags::SERIALIZE_TIME_MASK
               : 0);
      return create_tl_object<ton_api::validatorStats_stats_producer>(
          flags, validator_id.bits256_value(), block_status, candidate_id, create_tl_block_id(block_id),
          collated_data_hash, is_accepted, is_ours, got_block_at, got_block_by, got_submit_at, gen_utime, comment,
          collation_time, collated_at, collation_cached, self_collated, collator_node_id, validation_time, validated_at,
          validation_cached, approved_weight, approved_33pct_at, approved_66pct_at, std::move(approvers_str),
          signed_weight, signed_33pct_at, signed_66pct_at, std::move(signers_str), serialize_time, deserialize_time,
          serialized_size);
    }
  };
  struct Round {
    double started_at = -1.0;
    std::vector<Producer> producers;

    tl_object_ptr<ton_api::validatorStats_stats_round> tl() const {
      std::vector<tl_object_ptr<ton_api::validatorStats_stats_producer>> producers_tl;
      for (const auto &producer : producers) {
        producers_tl.push_back(producer.tl());
      }
      return create_tl_object<ton_api::validatorStats_stats_round>(started_at, std::move(producers_tl));
    }
  };

  ValidatorSessionId session_id = ValidatorSessionId::zero();
  PublicKeyHash self = PublicKeyHash::zero();
  BlockIdExt block_id{workchainIdNotYet, 0, 0, td::Bits256::zero(), td::Bits256::zero()};
  CatchainSeqno cc_seqno = 0;
  bool success = false;
  double timestamp = -1.0;
  PublicKeyHash creator = PublicKeyHash::zero();
  td::uint32 total_validators = 0;
  ValidatorWeight total_weight = 0;
  td::uint32 signatures = 0;
  ValidatorWeight signatures_weight = 0;
  td::uint32 approve_signatures = 0;
  ValidatorWeight approve_signatures_weight = 0;

  td::uint32 first_round = 0;
  std::vector<Round> rounds;

  void fix_block_ids() {
    for (auto &round : rounds) {
      for (auto &producer : round.producers) {
        producer.block_id.id = block_id.id;
      }
    }
  }

  tl_object_ptr<ton_api::validatorStats_stats> tl() const {
    std::vector<tl_object_ptr<ton_api::validatorStats_stats_round>> rounds_tl;
    for (const auto &round : rounds) {
      rounds_tl.push_back(round.tl());
    }
    int flags = success ? ton_api::validatorStats_stats::Flags::CREATOR_MASK : 0;
    return create_tl_object<ton_api::validatorStats_stats>(
        flags, session_id, self.bits256_value(), create_tl_block_id(block_id), cc_seqno, success, timestamp,
        creator.bits256_value(), total_validators, total_weight, signatures, signatures_weight, approve_signatures,
        approve_signatures_weight, first_round, std::move(rounds_tl));
  }
};

struct NewValidatorGroupStats {
  struct Node {
    PublicKeyHash id = PublicKeyHash::zero();
    PublicKey pubkey;
    adnl::AdnlNodeIdShort adnl_id = adnl::AdnlNodeIdShort::zero();
    ValidatorWeight weight = 0;
  };

  ValidatorSessionId session_id = ValidatorSessionId::zero();
  ShardIdFull shard{masterchainId};
  CatchainSeqno cc_seqno = 0;
  BlockSeqno last_key_block_seqno = 0;
  double started_at = -1.0;
  std::vector<BlockIdExt> prev;
  td::uint32 self_idx = 0;
  PublicKeyHash self = PublicKeyHash::zero();
  std::vector<Node> nodes{};

  tl_object_ptr<ton_api::validatorStats_newValidatorGroup> tl() const {
    std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>> prev_arr;
    for (const auto &p : prev) {
      prev_arr.push_back(create_tl_block_id(p));
    }
    std::vector<tl_object_ptr<ton_api::validatorStats_newValidatorGroup_node>> nodes_arr;
    for (const auto &node : nodes) {
      nodes_arr.push_back(create_tl_object<ton_api::validatorStats_newValidatorGroup_node>(
          node.id.bits256_value(), node.pubkey.tl(), node.adnl_id.bits256_value(), node.weight));
    }
    return create_tl_object<ton_api::validatorStats_newValidatorGroup>(
        session_id, create_tl_shard_id(shard), cc_seqno, last_key_block_seqno, started_at, std::move(prev_arr),
        self_idx, self.bits256_value(), std::move(nodes_arr));
  }
};

struct EndValidatorGroupStats {
  struct Node {
    PublicKeyHash id = PublicKeyHash::zero();
    td::uint32 catchain_blocks = 0;
  };

  ValidatorSessionId session_id = ValidatorSessionId::zero();
  double timestamp = -1.0;
  PublicKeyHash self = PublicKeyHash::zero();
  std::vector<Node> nodes{};

  tl_object_ptr<ton_api::validatorStats_endValidatorGroup> tl() const {
    std::vector<tl_object_ptr<ton_api::validatorStats_endValidatorGroup_node>> nodes_arr;
    for (const auto &node : nodes) {
      nodes_arr.push_back(create_tl_object<ton_api::validatorStats_endValidatorGroup_node>(node.id.bits256_value(),
                                                                                           node.catchain_blocks));
    }
    return create_tl_object<ton_api::validatorStats_endValidatorGroup>(session_id, timestamp, self.bits256_value(),
                                                                       std::move(nodes_arr));
  }
};

struct BlockSourceInfo {
  PublicKey source;
  BlockCandidatePriority priority;
};

}  // namespace validatorsession

}  // namespace ton
