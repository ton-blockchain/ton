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

#include "auto/tl/lite_api.hpp"
#include "crypto/common/refcnt.hpp"
#include "ton/ton-types.h"
#include "vm/cells.h"

#include "validator-set.h"

namespace block {

class BlockSignatureSet : public td::CntObject {
 public:
  virtual td::Result<ton::ValidatorWeight> check_signatures(td::Ref<ValidatorSet> vset,
                                                            ton::BlockIdExt block_id) const = 0;

  virtual size_t get_size() const = 0;
  virtual td::Result<ton::ValidatorWeight> get_weight(td::Ref<ValidatorSet> vset) const = 0;
  virtual bool is_ordinary() const {
    return false;
  }

  virtual td::Result<td::Ref<vm::Cell>> serialize(td::Ref<ValidatorSet> vset) const = 0;
  virtual ton::tl_object_ptr<ton::ton_api::tonNode_signatureSet_ordinary> tl() const = 0;
  virtual ton::tl_object_ptr<ton::lite_api::liteServer_signatureSet> tl_lite() const = 0;

  // ordinary signature set only (is_ordinary())
  virtual std::vector<ton::tl_object_ptr<ton::ton_api::tonNode_blockSignature>> tl_legacy() const {
    UNREACHABLE();
  }

  BlockSignatureSet(ton::CatchainSeqno cc_seqno, td::uint32 validator_set_hash)
      : cc_seqno_(cc_seqno), validator_set_hash_(validator_set_hash) {
  }
  ton::CatchainSeqno get_catchain_seqno() const {
    return cc_seqno_;
  }
  td::uint32 get_validator_set_hash() const {
    return validator_set_hash_;
  }

 protected:
  ton::CatchainSeqno cc_seqno_;
  td::uint32 validator_set_hash_;

 public:
  static td::Ref<BlockSignatureSet> create_ordinary(std::vector<ton::BlockSignature> signatures,
                                                    ton::CatchainSeqno cc_seqno, td::uint32 validator_set_hash);

  static td::Result<td::Ref<BlockSignatureSet>> fetch(td::Ref<vm::Cell> cell, ton::ValidatorWeight& total_weight);
  static td::Result<td::Ref<BlockSignatureSet>> fetch(td::Ref<vm::Cell> cell, td::Ref<ValidatorSet> vset);
  static td::Ref<BlockSignatureSet> fetch(
      const std::vector<ton::tl_object_ptr<ton::ton_api::tonNode_blockSignature>>& f, ton::CatchainSeqno cc_seqno,
      td::uint32 validator_set_hash);
  static td::Ref<BlockSignatureSet> fetch(const ton::tl_object_ptr<ton::ton_api::tonNode_signatureSet_ordinary>& f);
  static td::Ref<BlockSignatureSet> fetch(const ton::tl_object_ptr<ton::lite_api::liteServer_signatureSet>& f);

  static constexpr size_t MAX_SIGNATURES = 1024;
};

}  // namespace block
