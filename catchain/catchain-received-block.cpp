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
#include <set>

#include "catchain-received-block.hpp"
#include "catchain-receiver-source.h"

#include "auto/tl/ton_api.hpp"

namespace ton {

namespace catchain {

void CatChainReceivedBlockImpl::initialize(tl_object_ptr<ton_api::catchain_block> block, td::SharedSlice payload) {
  if (state_ != bs_none) {
    return;
  }

  payload_ = std::move(payload);
  CHECK(!payload_.empty());

  prev_ = dynamic_cast<CatChainReceivedBlockImpl *>(chain_->create_block(std::move(block->data_->prev_)));
  CHECK(prev_ != nullptr);
  for (auto &X : block->data_->deps_) {
    auto B = dynamic_cast<CatChainReceivedBlockImpl *>(chain_->create_block(std::move(X)));
    CHECK(B != nullptr);
    block_deps_.push_back(B);
  }
  signature_ = td::SharedSlice{block->signature_.as_slice()};

  state_ = bs_initialized;
  VLOG(CATCHAIN_DEBUG) << this << ": initialized payload_size=" << payload_.size();

  if (prev_->is_ill()) {
    set_ill();
    return;
  }
  for (CatChainReceivedBlockImpl *X : block_deps_) {
    if (X->is_ill()) {
      set_ill();
      return;
    }
  }

  td::uint32 pending_deps = 0;
  {
    if (!prev_->delivered()) {
      pending_deps++;
    } else {
      update_vt(prev_);
    }
    if (!prev_->delivered()) {
      prev_->add_rev_dep(this);
    }
  }
  for (CatChainReceivedBlockImpl *X : block_deps_) {
    if (!X->delivered()) {
      pending_deps++;
    } else {
      update_vt(X);
    }
    if (!X->delivered()) {
      X->add_rev_dep(this);
    }
  }
  pending_deps_ = pending_deps;
  if (pending_deps_ == 0 && in_db_) {
    schedule();
  }

  chain_->get_source(source_id_)->block_received(height_);
}

void CatChainReceivedBlockImpl::run() {
  if (is_ill()) {
    return;
  }
  if (state_ == bs_none) {
    return;
  }
  if (state_ == bs_delivered) {
    return;
  }
  CHECK(state_ == bs_initialized);
  CHECK(!pending_deps_);
  CHECK(in_db_);

  initialize_fork();
  pre_deliver();
  deliver();
}

void CatChainReceivedBlockImpl::initialize_fork() {
  CHECK(state_ == bs_initialized);
  CHECK(!fork_id_);
  CatChainReceiverSource *S = chain_->get_source(source_id_);
  if (height_ == 1) {
    fork_id_ = S->add_fork();
  } else {
    if (!prev_->next_) {
      prev_->next_ = this;
      fork_id_ = prev_->fork_id_;
    } else {
      fork_id_ = S->add_fork();
    }
  }

  if (vt_.size() < fork_id_ + 1) {
    vt_.resize(fork_id_ + 1, 0);
  }
  CHECK(vt_[fork_id_] < height_);
  vt_[fork_id_] = height_;
}

void CatChainReceivedBlockImpl::pre_deliver(ton_api::catchain_block_data_fork &b) {
  {
    td::Status S;
    S = chain_->validate_block_sync(b.left_);
    if (S.is_error()) {
      VLOG(CATCHAIN_WARNING) << this << ": incorrect fork blame: left is invalid: " << S.move_as_error();
      set_ill();
      return;
    }
    S = chain_->validate_block_sync(b.right_);
    if (S.is_error()) {
      VLOG(CATCHAIN_WARNING) << this << ": incorrect fork blame: right is invalid: " << S.move_as_error();
      set_ill();
      return;
    }
  }

  // block is incorrect, since blocks are
  if (b.left_->height_ != b.right_->height_ || b.left_->src_ != b.right_->src_ ||
      b.left_->data_hash_ == b.right_->data_hash_) {
    VLOG(CATCHAIN_WARNING) << this << ": incorrect fork blame: not a fork";
    set_ill();
    return;
  }

  CatChainReceiverSource *S = chain_->get_source(b.left_->src_);
  S->on_found_fork_proof(
      create_serialize_tl_object<ton_api::catchain_block_data_fork>(std::move(b.left_), std::move(b.right_)));
  S->blame(fork_id_, height_);
}

void CatChainReceivedBlockImpl::pre_deliver(ton_api::catchain_block_data_badBlock &b) {
}

void CatChainReceivedBlockImpl::pre_deliver(ton_api::catchain_block_data_nop &b) {
}

void CatChainReceivedBlockImpl::pre_deliver() {
  if (is_ill()) {
    return;
  }
  CHECK(state_ == bs_initialized);
  CHECK(pending_deps_ == 0);
  CHECK(in_db_);

  CatChainReceiverSource *M = chain_->get_source(source_id_);

  std::vector<CatChainBlockHeight> *d = prev_ ? &prev_->vt_ : nullptr;

  for (CatChainReceivedBlockImpl *X : block_deps_) {
    CatChainReceiverSource *S = chain_->get_source(X->get_source_id());
    const std::vector<td::uint32> &f = S->get_forks();
    if (d) {
      const std::vector<CatChainBlockHeight> &dd = *d;
      if (X->get_fork_id() < dd.size() && X->get_height() <= dd[X->get_fork_id()]) {
        VLOG(CATCHAIN_WARNING) << this << ": has direct dep from source " << X->get_source_id() << " and prev block "
                               << " has newer indirect dep";
        set_ill();
        return;
      }
    }
    if (S->blamed() && d) {
      const std::vector<CatChainBlockHeight> &dd = *d;
      for (td::uint32 x : f) {
        if (x != X->get_fork_id() && dd.size() > x && dd[x] > 0) {
          VLOG(CATCHAIN_WARNING) << this << ": has direct dep from source " << X->get_source_id() << " and prev block "
                                 << " has indirect dep to another fork of this source " << x << " " << X->get_fork_id()
                                 << " " << dd[x] << " " << f;
          M->blame(fork_id_, height_);
          set_ill();
          return;
        }
      }
      std::vector<CatChainBlockHeight> v = S->get_blamed_heights();

      for (size_t i = 0; i < v.size() && i < dd.size(); i++) {
        if (v[i] > 0 && dd[i] >= v[i]) {
          VLOG(CATCHAIN_WARNING) << this << ": has direct dep from source " << X->get_source_id() << " and prev block "
                                 << " has indirect dep to block f" << i << "@" << v[i]
                                 << " which is known to blame this source";
          M->blame(fork_id_, height_);
          set_ill();
          return;
        }
      }
    }
  }

  auto X = fetch_tl_object<ton_api::catchain_block_inner_Data>(payload_.as_slice(), true);
  if (X.is_error()) {
    is_custom_ = true;
  } else {
    ton_api::downcast_call(*X.move_as_ok(), [Self = this](auto &obj) { Self->pre_deliver(obj); });
  }
}

void CatChainReceivedBlockImpl::deliver() {
  if (is_ill()) {
    return;
  }
  CHECK(state_ == bs_initialized);
  CHECK(pending_deps_ == 0);
  CHECK(in_db_);

  chain_->deliver_block(this);

  state_ = bs_delivered;
  VLOG(CATCHAIN_DEBUG) << this << ": delivered";

  for (CatChainReceivedBlockImpl *B : rev_deps_) {
    B->dep_delivered(this);
  }
  rev_deps_.clear();

  chain_->get_source(source_id_)->block_delivered(height_);
}

void CatChainReceivedBlockImpl::set_ill() {
  if (state_ == bs_ill) {
    return;
  }
  VLOG(CATCHAIN_WARNING) << this << ": got ill";
  CatChainReceiverSource *M = chain_->get_source(source_id_);
  M->blame();
  state_ = bs_ill;
  for (CatChainReceivedBlockImpl *B : rev_deps_) {
    B->dep_ill(this);
  }
}

void CatChainReceivedBlockImpl::dep_ill(CatChainReceivedBlockImpl *block) {
  set_ill();
}

void CatChainReceivedBlockImpl::update_vt(CatChainReceivedBlockImpl *block) {
  const std::vector<CatChainBlockHeight> &d = block->vt_;
  if (d.size() > vt_.size()) {
    vt_.resize(d.size(), 0);
  }
  for (size_t i = 0; i < d.size(); i++) {
    if (vt_[i] < d[i]) {
      vt_[i] = d[i];
    }
  }
}

void CatChainReceivedBlockImpl::dep_delivered(CatChainReceivedBlockImpl *block) {
  if (is_ill()) {
    return;
  }
  CHECK(!block->is_ill());
  update_vt(block);
  pending_deps_--;
  if (pending_deps_ == 0 && in_db_) {
    schedule();
  }
}

void CatChainReceivedBlockImpl::written() {
  if (!in_db_) {
    in_db_ = true;
    if (pending_deps_ == 0) {
      schedule();
    }
  }
}

void CatChainReceivedBlockImpl::add_rev_dep(CatChainReceivedBlockImpl *block) {
  rev_deps_.push_back(block);
}

CatChainReceivedBlock *CatChainReceivedBlockImpl::get_prev() const {
  return prev_;
}

CatChainBlockHash CatChainReceivedBlockImpl::get_prev_hash() const {
  CHECK(prev_);
  return prev_->get_hash();
}

std::vector<CatChainBlockHash> CatChainReceivedBlockImpl::get_dep_hashes() const {
  std::vector<CatChainBlockHash> v;
  v.resize(block_deps_.size());
  for (size_t i = 0; i < v.size(); i++) {
    v[i] = block_deps_[i]->get_hash();
  }
  return v;
}

void CatChainReceivedBlockImpl::schedule() {
  chain_->run_block(this);
}

void CatChainReceivedBlockImpl::find_pending_deps(std::vector<CatChainBlockHash> &vec, td::uint32 max_size) const {
  if (height_ == 0 || is_ill() || delivered() || vec.size() == max_size) {
    return;
  }
  if (!initialized()) {
    vec.push_back(get_hash());
    return;
  }
  if (prev_) {
    prev_->find_pending_deps(vec, max_size);
  }
  for (CatChainReceivedBlockImpl *X : block_deps_) {
    X->find_pending_deps(vec, max_size);
  }
}

tl_object_ptr<ton_api::catchain_block_id> CatChainReceivedBlock::block_id(
    const CatChainReceiver *chain, const tl_object_ptr<ton_api::catchain_block> &block, const td::Slice &payload) {
  td::Bits256 hash = data_payload_hash(chain, block->data_, payload);
  return create_tl_object<ton_api::catchain_block_id>(block->incarnation_, chain->get_source_hash(block->src_).tl(),
                                                      block->height_, hash);
}
tl_object_ptr<ton_api::catchain_block_id> CatChainReceivedBlock::block_id(
    const CatChainReceiver *chain, const tl_object_ptr<ton_api::catchain_block_dep> &block) {
  return create_tl_object<ton_api::catchain_block_id>(
      chain->get_incarnation(), chain->get_source_hash(block->src_).tl(), block->height_, block->data_hash_);
}

CatChainBlockHash CatChainReceivedBlock::block_hash(const CatChainReceiver *chain,
                                                    const tl_object_ptr<ton_api::catchain_block> &block,
                                                    const td::Slice &payload) {
  return get_tl_object_sha_bits256(block_id(chain, block, payload));
}

CatChainBlockHash CatChainReceivedBlock::block_hash(const CatChainReceiver *chain,
                                                    const tl_object_ptr<ton_api::catchain_block_dep> &block) {
  return get_tl_object_sha_bits256(block_id(chain, block));
}

td::Status CatChainReceivedBlock::pre_validate_block(const CatChainReceiver *chain,
                                                     const tl_object_ptr<ton_api::catchain_block> &block,
                                                     const td::Slice &payload) {
  CHECK(block->incarnation_ == chain->get_incarnation());
  if (block->height_ <= 0) {
    return td::Status::Error(ErrorCode::protoviolation, std::string("bad height ") + std::to_string(block->height_));
  }
  if (block->src_ < 0 || static_cast<td::uint32>(block->src_) >= chain->get_sources_cnt()) {
    return td::Status::Error(ErrorCode::protoviolation, std::string("bad src ") + std::to_string(block->src_));
  }

  if (block->data_->prev_->src_ < 0) {
    return td::Status::Error(ErrorCode::protoviolation,
                             std::string("bad prev block src ") + std::to_string(block->data_->prev_->src_));
  }
  if (block->data_->deps_.size() > chain->opts().max_deps) {
    return td::Status::Error(ErrorCode::protoviolation, std::string("too many deps"));
  }
  auto prev_src = static_cast<td::uint32>(block->data_->prev_->src_);

  if (block->height_ > 1) {
    if (prev_src != static_cast<td::uint32>(block->src_)) {
      return td::Status::Error(ErrorCode::protoviolation,
                               std::string("bad prev block src ") + std::to_string(block->data_->prev_->src_));
    }
  } else {
    if (prev_src != chain->get_sources_cnt()) {
      return td::Status::Error(ErrorCode::protoviolation,
                               std::string("bad prev(first) block src ") + std::to_string(block->data_->prev_->src_));
    }
  }
  if (block->data_->prev_->height_ + 1 != block->height_) {
    return td::Status::Error(ErrorCode::protoviolation, std::string("bad prev block height ") +
                                                            std::to_string(block->data_->prev_->height_) + " (our " +
                                                            std::to_string(block->height_) + ")");
  }

  std::set<td::uint32> used;
  used.insert(block->src_);
  for (const auto &X : block->data_->deps_) {
    if (used.find(X->src_) != used.end()) {
      return td::Status::Error(ErrorCode::protoviolation, "two deps from same source");
    }
    used.insert(X->src_);
  }

  TRY_STATUS(chain->validate_block_sync(block->data_->prev_));
  for (const auto &X : block->data_->deps_) {
    TRY_STATUS(chain->validate_block_sync(X));
  }

  if (payload.empty()) {
    return td::Status::Error(ErrorCode::protoviolation, "empty payload");
  }

  return td::Status::OK();
}

td::Status CatChainReceivedBlock::pre_validate_block(const CatChainReceiver *chain,
                                                     const tl_object_ptr<ton_api::catchain_block_dep> &block) {
  if (block->height_ < 0) {
    return td::Status::Error(ErrorCode::protoviolation, std::string("bad height ") + std::to_string(block->height_));
  }
  if (block->height_ > 0) {
    if (block->src_ < 0 || static_cast<td::uint32>(block->src_) >= chain->get_sources_cnt()) {
      return td::Status::Error(ErrorCode::protoviolation, std::string("bad src ") + std::to_string(block->src_));
    }
  } else {
    if (block->src_ < 0 || static_cast<td::uint32>(block->src_) != chain->get_sources_cnt()) {
      return td::Status::Error(ErrorCode::protoviolation,
                               std::string("bad src (first block) ") + std::to_string(block->src_));
    }
    if (block->data_hash_ != chain->get_incarnation() || !block->signature_.empty()) {
      return td::Status::Error(ErrorCode::protoviolation, std::string("bad first block"));
    }
  }

  return td::Status::OK();
}

tl_object_ptr<ton_api::catchain_block> CatChainReceivedBlockImpl::export_tl() const {
  CHECK(initialized());
  CHECK(height_ > 0);
  std::vector<tl_object_ptr<ton_api::catchain_block_dep>> deps;

  for (CatChainReceivedBlockImpl *B : block_deps_) {
    deps.push_back(B->export_tl_dep());
  }
  CHECK(deps.size() <= chain_->opts().max_deps)

  return create_tl_object<ton_api::catchain_block>(
      chain_->get_incarnation(), source_id_, height_,
      create_tl_object<ton_api::catchain_block_data>(prev_->export_tl_dep(), std::move(deps)),
      signature_.clone_as_buffer_slice());
}

tl_object_ptr<ton_api::catchain_block_dep> CatChainReceivedBlockImpl::export_tl_dep() const {
  return create_tl_object<ton_api::catchain_block_dep>(source_id_, height_, data_payload_hash_,
                                                       signature_.clone_as_buffer_slice());
}

CatChainReceivedBlockImpl::CatChainReceivedBlockImpl(td::uint32 source_id, const CatChainBlockPayloadHash &hash,
                                                     CatChainReceiver *chain) {
  chain_ = chain;
  state_ = bs_delivered;
  fork_id_ = 0;
  source_id_ = source_id;
  height_ = 0;

  data_payload_hash_ = hash;
  block_id_hash_ = get_tl_object_sha_bits256(create_tl_object<ton_api::catchain_block_id>(
      chain->get_incarnation(), chain->get_incarnation(), height_, data_payload_hash_));
}

CatChainReceivedBlockImpl::CatChainReceivedBlockImpl(tl_object_ptr<ton_api::catchain_block> block,
                                                     td::SharedSlice payload, CatChainReceiver *chain) {
  chain_ = chain;
  data_payload_hash_ = data_payload_hash(chain, block->data_, payload);
  block_id_hash_ = get_tl_object_sha_bits256(create_tl_object<ton_api::catchain_block_id>(
      block->incarnation_, chain->get_source_hash(block->src_).tl(), block->height_, data_payload_hash_));
  height_ = block->height_;
  source_id_ = block->src_;
  CHECK(height_ <= get_max_block_height(chain->opts(), chain->get_sources_cnt()));

  CatChainReceiverSource *S = chain_->get_source(source_id_);
  S->on_new_block(this);

  initialize(std::move(block), std::move(payload));
}

CatChainReceivedBlockImpl::CatChainReceivedBlockImpl(tl_object_ptr<ton_api::catchain_block_dep> block,
                                                     CatChainReceiver *chain) {
  chain_ = chain;
  data_payload_hash_ = block->data_hash_;
  source_id_ = block->src_;
  signature_ = td::SharedSlice{block->signature_.as_slice()};
  block_id_hash_ = get_tl_object_sha_bits256(create_tl_object<ton_api::catchain_block_id>(
      chain_->get_incarnation(), chain_->get_source_hash(source_id_).tl(), block->height_, data_payload_hash_));
  height_ = block->height_;

  CatChainReceiverSource *S = chain_->get_source(source_id_);
  S->on_new_block(this);
}

std::unique_ptr<CatChainReceivedBlock> CatChainReceivedBlock::create(tl_object_ptr<ton_api::catchain_block> block,
                                                                     td::SharedSlice payload, CatChainReceiver *chain) {
  return std::make_unique<CatChainReceivedBlockImpl>(std::move(block), std::move(payload), chain);
}

std::unique_ptr<CatChainReceivedBlock> CatChainReceivedBlock::create(tl_object_ptr<ton_api::catchain_block_dep> block,
                                                                     CatChainReceiver *chain) {
  return std::make_unique<CatChainReceivedBlockImpl>(std::move(block), chain);
}

std::unique_ptr<CatChainReceivedBlock> CatChainReceivedBlock::create_root(td::uint32 source_id,
                                                                          CatChainSessionId session_id,
                                                                          CatChainReceiver *chain) {
  return std::make_unique<CatChainReceivedBlockImpl>(source_id, session_id, chain);
}

CatChainBlockPayloadHash CatChainReceivedBlock::data_payload_hash(
    const CatChainReceiver *chain, const tl_object_ptr<ton_api::catchain_block_data> &data, const td::Slice &payload) {
  td::Bits256 hash = sha256_bits256(payload);
  if (chain->opts().block_hash_covers_data) {
    td::Bits256 data_hash = get_tl_object_sha_bits256(data);
    char buf[32 * 2];
    CHECK(hash.as_array().size() == 32 && data_hash.as_array().size() == 32);
    std::copy(hash.as_array().begin(), hash.as_array().end(), buf);
    std::copy(data_hash.as_array().begin(), data_hash.as_array().end(), buf + 32);
    return sha256_bits256(td::Slice(buf, buf + 64));
  } else {
    return hash;
  }
}

}  // namespace catchain

}  // namespace ton
