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
#include "config.h"

namespace ton {

namespace validator {

class ProofLink : public td::CntObject {
 public:
  struct BasicHeaderInfo {
    UnixTime utime;
    LogicalTime end_lt;
    CatchainSeqno cc_seqno;
    td::uint32 validator_set_hash;
    BlockSeqno prev_key_mc_seqno;
  };
  virtual ~ProofLink() = default;
  virtual BlockIdExt block_id() const = 0;
  virtual td::BufferSlice data() const = 0;
  virtual td::Result<BlockSeqno> prev_key_mc_seqno() const = 0;
  virtual td::Result<td::Ref<ConfigHolder>> get_key_block_config() const = 0;
  virtual td::Result<BasicHeaderInfo> get_basic_header_info() const = 0;
};

class Proof : virtual public ProofLink {
 public:
  virtual ~Proof() = default;
  virtual td::Result<td::Ref<ProofLink>> export_as_proof_link() const = 0;
};

}  // namespace validator

}  // namespace ton
