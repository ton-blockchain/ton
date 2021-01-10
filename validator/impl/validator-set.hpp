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

#include "validator/interfaces/validator-set.h"
#include "validator/interfaces/signature-set.h"
#include "ton/ton-types.h"
#include "keys/encryptor.h"
#include "block/mc-config.h"

#include <map>

namespace ton {

namespace validator {

class ValidatorSetQ : public ValidatorSet {
 public:
  bool is_validator(NodeIdShort id) const override;
  CatchainSeqno get_catchain_seqno() const override {
    return cc_seqno_;
  }
  td::uint32 get_validator_set_hash() const override {
    return hash_;
  }
  ShardId get_validator_set_from() const override {
    return for_.shard;
  }
  ValidatorWeight get_total_weight() const {
    return total_weight_;
  }
  std::vector<ValidatorDescr> export_vector() const override;
  td::Result<ValidatorWeight> check_signatures(RootHash root_hash, FileHash file_hash,
                                               td::Ref<BlockSignatureSet> signatures) const override;
  td::Result<ValidatorWeight> check_approve_signatures(RootHash root_hash, FileHash file_hash,
                                                       td::Ref<BlockSignatureSet> signatures) const override;

  ValidatorSetQ* make_copy() const override;

  ValidatorSetQ(CatchainSeqno cc_seqno, ShardIdFull from, std::vector<ValidatorDescr> nodes);

 private:
  CatchainSeqno cc_seqno_;
  ShardIdFull for_;
  td::uint32 hash_;
  ValidatorWeight total_weight_;
  std::vector<ValidatorDescr> ids_;
  std::vector<std::pair<NodeIdShort, size_t>> ids_map_;

  const ValidatorDescr* find_validator(const NodeIdShort& id) const;
};

class ValidatorSetCompute {
 public:
  td::Ref<ValidatorSet> get_validator_set(ShardIdFull shard, UnixTime utime, CatchainSeqno cc) const;
  td::Ref<ValidatorSet> get_next_validator_set(ShardIdFull shard, UnixTime utime, CatchainSeqno cc) const;
  td::Status init(const block::Config* config);
  ValidatorSetCompute() = default;

 private:
  const block::Config* config_{nullptr};
  std::unique_ptr<block::ValidatorSet> cur_validators_, next_validators_;
  td::Ref<ValidatorSet> compute_validator_set(ShardIdFull shard, const block::ValidatorSet& vset, UnixTime time,
                                              CatchainSeqno seqno) const;
};

}  // namespace validator

}  // namespace ton
