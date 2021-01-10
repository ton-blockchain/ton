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
#include "validator/interfaces/config.h"

namespace ton {
namespace validator {
using td::Ref;

class ConfigHolderQ : public ConfigHolder {
  std::shared_ptr<block::Config> config_;
  std::shared_ptr<vm::StaticBagOfCellsDb> boc_;

 public:
  ConfigHolderQ() = default;
  ConfigHolderQ(std::shared_ptr<block::Config> config, std::shared_ptr<vm::StaticBagOfCellsDb> boc)
      : config_(std::move(config)), boc_(std::move(boc)) {
  }
  ConfigHolderQ(std::shared_ptr<block::Config> config) : config_(std::move(config)) {
  }
  const block::Config *get_config() const {
    return config_.get();
  }
  ConfigHolderQ *make_copy() const override {
    return new ConfigHolderQ(*this);
  }
  // if necessary, add more public methods providing interface to config_->...()
  td::Ref<ValidatorSet> get_total_validator_set(int next) const override;  // next = -1 -> prev, next = 0 -> cur
  td::Ref<ValidatorSet> get_validator_set(ShardIdFull shard, UnixTime utime, CatchainSeqno seqno) const override;
  std::pair<UnixTime, UnixTime> get_validator_set_start_stop(int next) const override;
};

}  // namespace validator

}  // namespace ton
