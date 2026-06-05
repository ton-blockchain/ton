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

#include "ton/ton-types.h"

namespace ton::validatorsession {

constexpr int VERBOSITY_NAME(VALIDATOR_SESSION_BENCHMARK) = verbosity_WARNING;

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
  bool use_quic = false;

  td::uint32 proto_version = 0;

  td::Bits256 get_hash() const;
};

}  // namespace ton::validatorsession
