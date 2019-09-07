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
#include "validator-set.hpp"
#include "auto/tl/ton_api.h"
#include "adnl/utils.hpp"

#include <set>

namespace ton {

namespace validator {

namespace dummy0 {

bool ValidatorSetImpl::is_validator(NodeIdShort id) const {
  return ids_map_.count(id) > 0;
}

td::Result<ValidatorWeight> ValidatorSetImpl::check_signatures(RootHash root_hash, FileHash file_hash,
                                                               td::Ref<BlockSignatureSet> signatures) const {
  auto &sigs = signatures->signatures();

  auto b = create_tl_object<ton_api::ton_blockId>(Bits256_2_UInt256(root_hash), Bits256_2_UInt256(file_hash));
  auto block = serialize_tl_object(b, true);

  ValidatorWeight weight = 0;

  std::set<NodeIdShort> nodes;
  for (auto &sig : sigs) {
    if (nodes.count(sig.node) == 1) {
      return td::Status::Error(ErrorCode::protoviolation, "duplicate node to sign");
    }
    nodes.insert(sig.node);

    auto it = ids_map_.find(sig.node);
    if (it == ids_map_.end()) {
      return td::Status::Error(ErrorCode::protoviolation, "unknown node to sign");
    }

    auto idx = it->second;
    TRY_STATUS(ids_[idx].encryptor->check_signature(block.as_slice(), sig.signature.as_slice()));
    weight += ids_[idx].weight;
  }

  if (weight * 3 <= total_weight_ * 2) {
    return td::Status::Error(ErrorCode::protoviolation, "too small sig weight");
  }
  return weight;
}

ValidatorSetImpl::ValidatorSetImpl(CatchainSeqno cc_seqno, ShardId from,
                                   std::vector<std::pair<ValidatorFullId, ValidatorWeight>> nodes)
    : cc_seqno_(cc_seqno), from_(from) {
  total_weight_ = 0;

  std::vector<tl_object_ptr<ton_api::test0_validatorSetItem>> s_vec;

  for (auto &n : nodes) {
    auto idx = ids_.size();
    auto id = n.first.short_id();
    s_vec.emplace_back(create_tl_object<ton_api::test0_validatorSetItem>(Bits256_2_UInt256(id), n.second));
    CHECK(ids_map_.count(id) == 0);
    total_weight_ += n.second;
    auto E = n.first.create_encryptor().move_as_ok();
    ids_.emplace_back(ValidatorSetMember{n.first, n.second, std::move(E)});
    ids_map_.emplace(id, idx);
  }

  auto obj = create_tl_object<ton_api::test0_validatorSet>(cc_seqno_, std::move(s_vec));
  auto B = serialize_tl_object(obj, true);
  hash_ = td::crc32c(B.as_slice());
}

ValidatorSetImpl *ValidatorSetImpl::make_copy() const {
  return new ValidatorSetImpl{cc_seqno_, from_, export_vector()};
}

std::vector<std::pair<PublicKey, ValidatorWeight>> ValidatorSetImpl::export_tl_vector() const {
  std::vector<std::pair<PublicKey, ValidatorWeight>> vec;
  for (auto &v : ids_) {
    vec.emplace_back(v.id, v.weight);
  }

  return vec;
}

std::vector<std::pair<ValidatorFullId, ValidatorWeight>> ValidatorSetImpl::export_vector() const {
  std::vector<std::pair<ValidatorFullId, ValidatorWeight>> vec;
  for (auto &v : ids_) {
    vec.emplace_back(v.id, v.weight);
  }

  return vec;
}

}  // namespace dummy0

}  // namespace validator

}  // namespace ton
