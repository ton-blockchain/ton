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

#include "validator-full-id.h"
#include "signature-set.h"
#include "ton/ton-types.h"
#include "adnl/adnl-node-id.hpp"

namespace ton {

namespace validator {

class ValidatorSet : public td::CntObject {
 public:
  virtual ~ValidatorSet() = default;
  virtual bool is_validator(NodeIdShort id) const = 0;
  virtual CatchainSeqno get_catchain_seqno() const = 0;
  virtual td::uint32 get_validator_set_hash() const = 0;
  virtual ShardId get_validator_set_from() const = 0;
  virtual std::vector<ValidatorDescr> export_vector() const = 0;
  virtual td::Result<ValidatorWeight> check_signatures(RootHash root_hash, FileHash file_hash,
                                                       td::Ref<BlockSignatureSet> signatures) const = 0;
  virtual td::Result<ValidatorWeight> check_approve_signatures(RootHash root_hash, FileHash file_hash,
                                                               td::Ref<BlockSignatureSet> signatures) const = 0;
};

}  // namespace validator

}  // namespace ton
