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
#include "signature-set.hpp"
#include "auto/tl/ton_api.hpp"
#include "adnl/utils.hpp"

namespace ton {

namespace validator {

namespace dummy0 {

td::BufferSlice BlockSignatureSetImpl::serialize() const {
  std::vector<tl_object_ptr<ton_api::tonNode_blockSignature>> sigs;
  for (auto &s : signatures()) {
    sigs.emplace_back(
        create_tl_object<ton_api::tonNode_blockSignature>(Bits256_2_UInt256(s.node), s.signature.clone()));
  }
  auto obj = create_tl_object<ton_api::test0_blockSignatures>(std::move(sigs));
  return serialize_tl_object(obj, true);
}

td::Ref<BlockSignatureSet> BlockSignatureSetImpl::fetch(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::test0_blockSignatures>(std::move(data), true);
  auto obj = F.move_as_ok();

  std::vector<BlockSignature> sigs;
  for (auto &s : obj->signatures_) {
    sigs.emplace_back(BlockSignature{UInt256_2_Bits256(s->who_), std::move(s->signature_)});
  }

  return td::Ref<BlockSignatureSetImpl>{true, std::move(sigs)};
}

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
