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
#include "tl-utils/tl-utils.hpp"

#include "validator-session-types.h"

namespace ton::validatorsession {

ValidatorSessionOptions::ValidatorSessionOptions(const ValidatorSessionConfig &conf)
    : catchain_opts(conf.catchain_opts)
    , round_candidates(conf.round_candidates)
    , next_candidate_delay(conf.next_candidate_delay)
    , round_attempt_duration(conf.round_attempt_duration)
    , max_round_attempts(conf.max_round_attempts)
    , max_block_size(conf.max_block_size)
    , max_collated_data_size(conf.max_collated_data_size)
    , new_catchain_ids(conf.new_catchain_ids)
    , use_quic(conf.use_quic)
    , proto_version(conf.proto_version) {
}

td::Bits256 ValidatorSessionOptions::get_hash() const {
  if (proto_version == 0) {
    if (!new_catchain_ids) {
      return create_hash_tl_object<ton_api::validatorSession_config>(
          catchain_opts.idle_timeout, catchain_opts.max_deps, round_candidates, next_candidate_delay,
          round_attempt_duration, max_round_attempts, max_block_size, max_collated_data_size);
    } else {
      return create_hash_tl_object<ton_api::validatorSession_configNew>(
          catchain_opts.idle_timeout, catchain_opts.max_deps, round_candidates, next_candidate_delay,
          round_attempt_duration, max_round_attempts, max_block_size, max_collated_data_size, new_catchain_ids);
    }
  } else if (proto_version == 1) {
    return create_hash_tl_object<ton_api::validatorSession_configVersioned>(
        catchain_opts.idle_timeout, catchain_opts.max_deps, round_candidates, next_candidate_delay,
        round_attempt_duration, max_round_attempts, max_block_size, max_collated_data_size, proto_version);
  } else {
    return create_hash_tl_object<ton_api::validatorSession_configVersionedV2>(
        create_tl_object<ton_api::validatorSession_catchainOptions>(
            catchain_opts.idle_timeout, catchain_opts.max_deps,
            static_cast<td::uint32>(catchain_opts.max_serialized_block_size), catchain_opts.block_hash_covers_data,
            static_cast<td::uint32>(catchain_opts.max_block_height_coeff), catchain_opts.debug_disable_db),
        round_candidates, next_candidate_delay, round_attempt_duration, max_round_attempts, max_block_size,
        max_collated_data_size, proto_version);
  }
}

}  // namespace ton::validatorsession
