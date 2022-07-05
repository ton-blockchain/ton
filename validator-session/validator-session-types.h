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
  td::uint32 round;
  td::uint32 total_validators;
  ValidatorWeight total_weight;
  td::uint32 signatures;
  ValidatorWeight signatures_weight;
  td::uint32 approve_signatures;
  ValidatorWeight approve_signatures_weight;

  PublicKeyHash creator;
  std::vector<PublicKeyHash> producers;
};

}  // namespace validatorsession

}  // namespace ton
