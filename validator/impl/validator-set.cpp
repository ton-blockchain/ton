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
#include "validator-set.hpp"
#include "auto/tl/ton_api.h"
// #include "adnl/utils.hpp"
#include "block/block.h"

#include <set>

namespace ton {

namespace validator {
using td::Ref;

const ValidatorDescr *ValidatorSetQ::find_validator(const NodeIdShort &id) const {
  auto it =
      std::lower_bound(ids_map_.begin(), ids_map_.end(), id, [](const auto &p, const auto &x) { return p.first < x; });
  return it < ids_map_.end() && it->first == id ? &ids_[it->second] : nullptr;
}

bool ValidatorSetQ::is_validator(NodeIdShort id) const {
  return find_validator(id);
}

td::Result<ValidatorWeight> ValidatorSetQ::check_signatures(RootHash root_hash, FileHash file_hash,
                                                            td::Ref<BlockSignatureSet> signatures) const {
  auto &sigs = signatures->signatures();

  auto block = create_serialize_tl_object<ton_api::ton_blockId>(root_hash, file_hash);

  ValidatorWeight weight = 0;

  std::set<NodeIdShort> nodes;
  for (auto &sig : sigs) {
    if (nodes.count(sig.node) == 1) {
      return td::Status::Error(ErrorCode::protoviolation, "duplicate node to sign");
    }
    nodes.insert(sig.node);

    auto vdescr = find_validator(sig.node);
    if (!vdescr) {
      return td::Status::Error(ErrorCode::protoviolation, "unknown node to sign");
    }

    auto E = ValidatorFullId{vdescr->key}.create_encryptor().move_as_ok();
    TRY_STATUS(E->check_signature(block.as_slice(), sig.signature.as_slice()));
    weight += vdescr->weight;
  }

  if (weight * 3 <= total_weight_ * 2) {
    return td::Status::Error(ErrorCode::protoviolation, "too small sig weight");
  }
  return weight;
}

td::Result<ValidatorWeight> ValidatorSetQ::check_approve_signatures(RootHash root_hash, FileHash file_hash,
                                                                    td::Ref<BlockSignatureSet> signatures) const {
  auto &sigs = signatures->signatures();

  auto block = create_serialize_tl_object<ton_api::ton_blockIdApprove>(root_hash, file_hash);

  ValidatorWeight weight = 0;

  std::set<NodeIdShort> nodes;
  for (auto &sig : sigs) {
    if (nodes.count(sig.node) == 1) {
      return td::Status::Error(ErrorCode::protoviolation, "duplicate node to sign");
    }
    nodes.insert(sig.node);

    auto vdescr = find_validator(sig.node);
    if (!vdescr) {
      return td::Status::Error(ErrorCode::protoviolation, "unknown node to sign");
    }

    auto E = ValidatorFullId{vdescr->key}.create_encryptor().move_as_ok();
    TRY_STATUS(E->check_signature(block.as_slice(), sig.signature.as_slice()));
    weight += vdescr->weight;
  }

  if (weight * 3 <= total_weight_ * 2) {
    return td::Status::Error(ErrorCode::protoviolation, "too small sig weight");
  }
  return weight;
}

ValidatorSetQ::ValidatorSetQ(CatchainSeqno cc_seqno, ShardIdFull from, std::vector<ValidatorDescr> nodes)
    : cc_seqno_(cc_seqno), for_(from), ids_(std::move(nodes)) {
  total_weight_ = 0;

  ids_map_.reserve(ids_.size());

  for (std::size_t i = 0; i < ids_.size(); i++) {
    total_weight_ += ids_[i].weight;
    ids_map_.emplace_back(ValidatorFullId{ids_[i].key}.short_id(), i);
  }

  std::sort(ids_map_.begin(), ids_map_.end());
  for (std::size_t i = 1; i < ids_map_.size(); i++) {
    CHECK(ids_map_[i - 1].first != ids_map_[i].first);
  }

  hash_ = block::compute_validator_set_hash(cc_seqno, from, ids_);
}

ValidatorSetQ *ValidatorSetQ::make_copy() const {
  return new ValidatorSetQ{*this};
}

std::vector<ValidatorDescr> ValidatorSetQ::export_vector() const {
  return ids_;
}

td::Status ValidatorSetCompute::init(const block::Config *config) {
  config_ = nullptr;
  cur_validators_.reset();
  next_validators_.reset();
  if (!config) {
    return td::Status::Error("null configuration pointer passed to ValidatorSetCompute");
  }
  config_ = config;
  auto cv_root = config_->get_config_param(34);
  if (cv_root.not_null()) {
    TRY_RESULT(validators, block::Config::unpack_validator_set(std::move(cv_root)));
    cur_validators_ = std::move(validators);
  }
  auto nv_root = config_->get_config_param(36);
  if (nv_root.not_null()) {
    TRY_RESULT(validators, block::Config::unpack_validator_set(std::move(nv_root)));
    next_validators_ = std::move(validators);
  }
  return td::Status::OK();
}

Ref<ValidatorSet> ValidatorSetCompute::compute_validator_set(ShardIdFull shard, const block::ValidatorSet &vset,
                                                             UnixTime time, CatchainSeqno ccseqno) const {
  if (!config_) {
    return {};
  }
  LOG(DEBUG) << "in compute_validator_set() for " << shard.to_str();
  auto nodes = config_->compute_validator_set(shard, vset, time, ccseqno);
  if (nodes.empty()) {
    LOG(ERROR) << "compute_validator_set() for " << shard.to_str() << "," << time << "," << ccseqno
               << " returned empty list";
    return {};
  }
  return Ref<ValidatorSetQ>{true, ccseqno, shard, std::move(nodes)};
}

Ref<ValidatorSet> ValidatorSetCompute::get_validator_set(ShardIdFull shard, UnixTime utime, CatchainSeqno cc) const {
  if (!config_ || !cur_validators_) {
    LOG(ERROR) << "ValidatorSetCompute::get_validator_set() : no config or no cur_validators";
    return {};
  }
  return compute_validator_set(shard, *cur_validators_, utime, cc);
}

Ref<ValidatorSet> ValidatorSetCompute::get_next_validator_set(ShardIdFull shard, UnixTime utime,
                                                              CatchainSeqno cc) const {
  if (!config_ || !cur_validators_) {
    LOG(ERROR) << "ValidatorSetCompute::get_next_validator_set() : no config or no cur_validators";
    return {};
  }
  if (!next_validators_) {
    return compute_validator_set(shard, *cur_validators_, utime, cc + 1);
  }
  bool is_mc = shard.is_masterchain();
  auto ccv_cfg = config_->get_catchain_validators_config();
  unsigned cc_lifetime = is_mc ? ccv_cfg.mc_cc_lifetime : ccv_cfg.shard_cc_lifetime;
  if (next_validators_->utime_since > (utime / cc_lifetime + 1) * cc_lifetime) {
    return compute_validator_set(shard, *cur_validators_, utime, cc + 1);
  } else {
    return compute_validator_set(shard, *next_validators_, utime, cc + 1);
  }
}

}  // namespace validator

}  // namespace ton
