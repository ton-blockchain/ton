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

    Copyright 2017-2019 Telegram Systems LLP
*/

#include "config.hpp"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"
#include "validator-set.hpp"

namespace ton {

namespace validator {

td::Ref<ValidatorSet> ConfigHolderQ::get_total_validator_set(int next) const {
  if (!config_) {
    LOG(ERROR) << "MasterchainStateQ::get_total_validator_set() : no config";
    return {};
  }
  auto nodes = config_->compute_total_validator_set(next);
  if (nodes.empty()) {
    return {};
  }
  return Ref<ValidatorSetQ>{true, 0, ton::ShardIdFull{}, std::move(nodes)};
}

td::Ref<ValidatorSet> ConfigHolderQ::get_validator_set(ShardIdFull shard, UnixTime utime,
                                                       CatchainSeqno cc_seqno) const {
  if (!config_) {
    LOG(ERROR) << "MasterchainStateQ::get_validator_set() : no config";
    return {};
  }
  auto nodes = config_->compute_validator_set(shard, utime, cc_seqno);
  if (nodes.empty()) {
    return {};
  }
  return Ref<ValidatorSetQ>{true, cc_seqno, shard, std::move(nodes)};
}

std::pair<UnixTime, UnixTime> ConfigHolderQ::get_validator_set_start_stop(int next) const {
  if (!config_) {
    LOG(ERROR) << "MasterchainStateQ::get_validator_set_start_stop() : no config";
    return {};
  }
  return config_->get_validator_set_start_stop(next);
}

}  // namespace validator

}  // namespace ton
