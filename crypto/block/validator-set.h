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
#pragma once

#include "keys/encryptor.h"
#include "ton/ton-types.h"

namespace block {

class Config;
struct TotalValidatorSet;

class ValidatorSet : public td::CntObject {
 public:
  const ton::ValidatorDescr* get_validator(const ton::NodeIdShort& id) const;
  bool is_validator(ton::NodeIdShort id) const;
  ton::CatchainSeqno get_catchain_seqno() const {
    return cc_seqno_;
  }
  td::uint32 get_validator_set_hash() const {
    return hash_;
  }
  ton::ShardIdFull get_shard() const {
    return for_;
  }
  ton::ValidatorWeight get_total_weight() const {
    return total_weight_;
  }
  std::vector<ton::ValidatorDescr> export_vector() const;
  ValidatorSet* make_copy() const override;
  ValidatorSet(ton::CatchainSeqno cc_seqno, ton::ShardIdFull from, std::vector<ton::ValidatorDescr> nodes);

 private:
  ton::CatchainSeqno cc_seqno_;
  ton::ShardIdFull for_;
  td::uint32 hash_;
  ton::ValidatorWeight total_weight_;
  std::vector<ton::ValidatorDescr> ids_;
  std::vector<std::pair<ton::NodeIdShort, size_t>> ids_map_;
};

class ValidatorSetCompute {
 public:
  td::Ref<ValidatorSet> get_validator_set(ton::ShardIdFull shard, ton::UnixTime utime, ton::CatchainSeqno cc) const;
  td::Ref<ValidatorSet> get_next_validator_set(ton::ShardIdFull shard, ton::UnixTime utime,
                                               ton::CatchainSeqno cc) const;
  td::Status init(const Config* config);
  ValidatorSetCompute() = default;

 private:
  const Config* config_{nullptr};
  std::shared_ptr<TotalValidatorSet> cur_validators_, next_validators_;
  td::Ref<ValidatorSet> compute_validator_set(ton::ShardIdFull shard, const TotalValidatorSet& vset, ton::UnixTime time,
                                              ton::CatchainSeqno cc_seqno) const;
};

}  // namespace block
