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

#include "validator/interfaces/validator-set.h"
#include "validator/interfaces/signature-set.h"
#include "keys/encryptor.h"

#include <map>

namespace ton {

namespace validator {

namespace dummy0 {

class ValidatorSetImpl : public ValidatorSet {
 private:
  struct ValidatorSetMember {
    ValidatorFullId id;
    ValidatorWeight weight;
    std::unique_ptr<Encryptor> encryptor;
  };

 public:
  bool is_validator(NodeIdShort id) const override;
  CatchainSeqno get_catchain_seqno() const override {
    return cc_seqno_;
  }
  td::uint32 get_validator_set_hash() const override {
    return hash_;
  }
  ShardId get_validator_set_from() const override {
    return from_;
  }
  std::vector<std::pair<ValidatorFullId, ValidatorWeight>> export_vector() const override;
  std::vector<std::pair<PublicKey, ValidatorWeight>> export_tl_vector() const override;
  td::Result<ValidatorWeight> check_signatures(RootHash root_hash, FileHash file_hash,
                                               td::Ref<BlockSignatureSet> signatures) const override;

  ValidatorSetImpl *make_copy() const override;

  ValidatorSetImpl(UnixTime ts, ShardId from_, std::vector<std::pair<ValidatorFullId, ValidatorWeight>> nodes);

 private:
  CatchainSeqno cc_seqno_;
  ShardId from_;
  td::uint32 hash_;
  ValidatorWeight total_weight_;
  std::vector<ValidatorSetMember> ids_;
  std::map<NodeIdShort, size_t> ids_map_;
};

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
