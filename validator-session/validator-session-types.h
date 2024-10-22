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

#include "td/utils/int_types.h"
#include "crypto/common/bitstring.h"
#include "adnl/adnl-node-id.hpp"
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

  struct Producer {
    PublicKeyHash id = PublicKeyHash::zero();
    ValidatorSessionCandidateId candidate_id = ValidatorSessionCandidateId::zero();
    int block_status = status_none;
    double block_timestamp = -1.0;
    td::Bits256 root_hash = td::Bits256::zero();
    td::Bits256 file_hash = td::Bits256::zero();
    std::string comment;

    bool is_accepted = false;
    bool is_ours = false;
    double got_submit_at = -1.0;
    double collation_time = -1.0;
    double validation_time = -1.0;
    double collated_at = -1.0;
    double validated_at = -1.0;
    bool collation_cached = false;
    bool validation_cached = false;
    double gen_utime = -1.0;

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
  };
  struct Round {
    double timestamp = -1.0;
    std::vector<Producer> producers;
  };

  td::uint32 first_round;
  std::vector<Round> rounds;

  bool success = false;
  ValidatorSessionId session_id = ValidatorSessionId::zero();
  CatchainSeqno cc_seqno = 0;
  double timestamp = -1.0;
  PublicKeyHash self = PublicKeyHash::zero();
  PublicKeyHash creator = PublicKeyHash::zero();
  td::uint32 total_validators = 0;
  ValidatorWeight total_weight = 0;
  td::uint32 signatures = 0;
  ValidatorWeight signatures_weight = 0;
  td::uint32 approve_signatures = 0;
  ValidatorWeight approve_signatures_weight = 0;
};

struct NewValidatorGroupStats {
  struct Node {
    PublicKeyHash id = PublicKeyHash::zero();
    ValidatorWeight weight = 0;
  };

  ValidatorSessionId session_id = ValidatorSessionId::zero();
  ShardIdFull shard{masterchainId};
  CatchainSeqno cc_seqno = 0;
  BlockSeqno last_key_block_seqno = 0;
  double timestamp = -1.0;
  td::uint32 self_idx = 0;
  std::vector<Node> nodes;
};

struct EndValidatorGroupStats {
  struct Node {
    PublicKeyHash id = PublicKeyHash::zero();
    td::uint32 catchain_blocks = 0;
  };

  ValidatorSessionId session_id = ValidatorSessionId::zero();
  double timestamp = -1.0;
  std::vector<Node> nodes;
};

struct BlockSourceInfo {
  td::uint32 round, first_block_round;
  PublicKey source;
  td::int32 source_priority;
};

}  // namespace validatorsession

}  // namespace ton
