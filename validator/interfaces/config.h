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
#pragma once

#include "ton/ton-types.h"
#include "crypto/common/refcnt.hpp"
#include "validator-set.h"
#include "crypto/block/mc-config.h"

namespace ton {

namespace validator {

using McShardHash = block::McShardHashI;

class ConfigHolder : public td::CntObject {
 public:
  virtual ~ConfigHolder() = default;

  virtual td::Ref<ValidatorSet> get_total_validator_set(int next) const = 0;  // next = -1 -> prev, next = 0 -> cur
  virtual td::Ref<ValidatorSet> get_validator_set(ShardIdFull shard, UnixTime utime, CatchainSeqno seqno) const = 0;
  virtual std::pair<UnixTime, UnixTime> get_validator_set_start_stop(int next) const = 0;
};

}  // namespace validator

}  // namespace ton
