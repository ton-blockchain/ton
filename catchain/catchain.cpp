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
#include "catchain-types.h"
#include "catchain.hpp"

#include <utility>
#include "catchain-receiver.h"

namespace ton {

namespace catchain {

void CatChainImpl::send_process() {
  CHECK(receiver_started_);

  std::vector<CatChainBlock *> v;
  std::vector<CatChainBlockHash> w;
  while (top_blocks_.size() > 0 && v.size() < opts_.max_deps) {
    CatChainBlock *B = *top_blocks_.get_random();
    CHECK(B != nullptr);
    top_blocks_.remove(B->hash());
    CHECK(B->source() < sources_.size());
    if (!blamed_sources_[B->source()]) {
      w.push_back(B->hash());
      v.push_back(B);
      set_processed(B);
    }
  }

  process_deps_ = std::move(w);
  VLOG(CATCHAIN_INFO) << this << ": creating block. deps=" << process_deps_;
  callback_->process_blocks(std::move(v));
  VLOG(CATCHAIN_INFO) << this << ": sent creating block";
}

void CatChainImpl::send_preprocess(CatChainBlock *block) {
  if (block->preprocess_is_sent()) {
    return;
  }
  CatChainBlock *prev = block->prev();
  if (prev) {
    send_preprocess(prev);
  }

  const std::vector<CatChainBlock *> &deps = block->deps();
  for (CatChainBlock *X : deps) {
    send_preprocess(X);
  }

  block->preprocess_sent();
  VLOG(CATCHAIN_INFO) << this << ": preprocessing block " << block->hash() << " src=" << block->source();
  callback_->preprocess_block(block);
  VLOG(CATCHAIN_INFO) << this << ": sent preprocessing block " << block->hash() << " src=" << block->source();
}

void CatChainImpl::set_processed(CatChainBlock *block) {
  if (block->is_processed()) {
    return;
  }
  CatChainBlock *prev = block->prev();
  if (prev) {
    set_processed(prev);
  }

  const std::vector<CatChainBlock *> &deps = block->deps();
  for (CatChainBlock *X : deps) {
    set_processed(X);
  }

  block->set_processed();
}

void CatChainImpl::processed_block(td::BufferSlice payload) {
  CHECK(receiver_started_);
  VLOG(CATCHAIN_INFO) << this << ": created block. deps=" << process_deps_ << " payload_size=" << payload.size();
  td::actor::send_closure(receiver_, &CatChainReceiverInterface::add_block, std::move(payload),
                          std::move(process_deps_));
  CHECK(active_process_);
  if (top_blocks_.size() > 0 || force_process_) {
    force_process_ = false;
    send_process();
  } else {
    active_process_ = false;
    VLOG(CATCHAIN_INFO) << this << ": finished processing";
    callback_->finished_processing();
    VLOG(CATCHAIN_INFO) << this << ": sent finished processing";
    alarm_timestamp() = td::Timestamp::in(opts_.idle_timeout);
  }
}

void CatChainImpl::need_new_block(td::Timestamp t) {
  if (!receiver_started_) {
    return;
  }
  if (!force_process_) {
    VLOG(CATCHAIN_INFO) << this << ": forcing creation of new block";
  }
  force_process_ = true;
  if (!active_process_) {
    alarm_timestamp().relax(t);
  }
}

void CatChainImpl::on_new_block(td::uint32 src_id, td::uint32 fork, CatChainBlockHash hash, CatChainBlockHeight height,
                                CatChainBlockHash prev, std::vector<CatChainBlockHash> deps,
                                std::vector<CatChainBlockHeight> vt, td::SharedSlice data) {
  VLOG(CATCHAIN_DEBUG) << this << ": new block " << hash;
  if (top_blocks_.size() == 0 && !active_process_ && receiver_started_) {
    alarm_timestamp().relax(td::Timestamp::in(opts_.idle_timeout));
  }

  CatChainBlock *p = nullptr;
  if (!prev.is_zero()) {
    p = get_block(prev);
    CHECK(p != nullptr);
    if (top_blocks_.exists(prev)) {
      top_blocks_.remove(prev);
    }
  }

  CHECK(src_id < sources_.size());
  std::vector<CatChainBlock *> v;
  v.resize(deps.size());
  for (size_t i = 0; i < deps.size(); i++) {
    if (!blamed_sources_[src_id] && top_blocks_.exists(deps[i])) {
      top_blocks_.remove(deps[i]);
    }
    v[i] = get_block(deps[i]);
    CHECK(v[i] != nullptr);
  }

  CHECK(height <= get_max_block_height(opts_, sources_.size()));
  PublicKeyHash src_hash = sources_[src_id];
  blocks_[hash] =
      CatChainBlock::create(src_id, fork, src_hash, height, hash, std::move(data), p, std::move(v), std::move(vt));

  CatChainBlock *B = get_block(hash);
  CHECK(B != nullptr);

  if (!blamed_sources_[src_id]) {
    send_preprocess(B);
    top_source_blocks_[src_id] = B;

    if (src_id != local_idx_) {
      top_blocks_.insert(B->hash(), B);
    }

    if (top_blocks_.size() == 0 && !active_process_ && receiver_started_) {
      alarm_timestamp().relax(td::Timestamp::in(opts_.idle_timeout));
    }
  }
}

void CatChainImpl::on_blame(td::uint32 src_id) {
  if (blamed_sources_[src_id]) {
    return;
  }
  blamed_sources_[src_id] = true;
  top_source_blocks_[src_id] = nullptr;

  // recompute top blocks
  top_blocks_.reset();
  auto size = static_cast<td::uint32>(sources_.size());
  for (td::uint32 i = 0; i < size; i++) {
    if (!blamed_sources_[i] && top_source_blocks_[i] && i != local_idx_) {
      CatChainBlock *B = top_source_blocks_[i];
      bool f = true;
      if (B->is_processed()) {
        continue;
      }
      for (td::uint32 j = 0; j < size; j++) {
        if (i != j && !blamed_sources_[j] && top_source_blocks_[j]) {
          if (top_source_blocks_[j]->is_descendant_of(B)) {
            f = false;
            break;
          }
        }
      }
      if (f) {
        top_blocks_.insert(B->hash(), B);
      }
    }
  }
}

void CatChainImpl::on_custom_query(const PublicKeyHash &src, td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  callback_->process_query(src, std::move(data), std::move(promise));
}

void CatChainImpl::on_broadcast(const PublicKeyHash &src, td::BufferSlice data) {
  VLOG(CATCHAIN_INFO) << this << ": processing broadcast";
  callback_->process_broadcast(src, std::move(data));
  VLOG(CATCHAIN_INFO) << this << ": sent processing broadcast";
}

void CatChainImpl::on_receiver_started() {
  receiver_started_ = true;
  callback_->started();
  CHECK(!active_process_);
  active_process_ = true;
  send_process();
}

CatChainImpl::CatChainImpl(std::unique_ptr<Callback> callback, const CatChainOptions &opts,
                           td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                           td::actor::ActorId<overlay::Overlays> overlay_manager, std::vector<CatChainNode> ids,
                           const PublicKeyHash &local_id, const CatChainSessionId &unique_hash,
                           std::string db_root, std::string db_suffix, bool allow_unsafe_self_blocks_resync)
    : opts_(opts)
    , unique_hash_(unique_hash)
    , db_root_(std::move(db_root))
    , db_suffix_(std::move(db_suffix))
    , allow_unsafe_self_blocks_resync_(allow_unsafe_self_blocks_resync) {
  callback_ = std::move(callback);
  sources_.resize(ids.size());
  for (size_t i = 0; i < ids.size(); i++) {
    sources_[i] = ids[i].pub_key.compute_short_id();
    if (sources_[i] == local_id) {
      local_idx_ = static_cast<td::uint32>(i);
    }
  }
  blamed_sources_.resize(ids.size(), false);
  top_source_blocks_.resize(ids.size(), nullptr);

  args_ = std::make_unique<Args>(std::move(keyring), std::move(adnl), std::move(overlay_manager), std::move(ids),
                                 local_id, unique_hash);
}

void CatChainImpl::alarm() {
  alarm_timestamp() = td::Timestamp::never();
  if (!active_process_) {
    active_process_ = true;
    send_process();
  }
}

void CatChainImpl::start_up() {
  class ChainCb : public CatChainReceiverInterface::Callback {
   public:
    void new_block(td::uint32 src_id, td::uint32 fork_id, CatChainBlockHash hash, CatChainBlockHeight height,
                   CatChainBlockHash prev, std::vector<CatChainBlockHash> deps, std::vector<CatChainBlockHeight> vt,
                   td::SharedSlice data) override {
      td::actor::send_closure(id_, &CatChainImpl::on_new_block, src_id, fork_id, hash, height, prev, std::move(deps),
                              std::move(vt), std::move(data));
    }
    void blame(td::uint32 src_id) override {
      td::actor::send_closure(id_, &CatChainImpl::on_blame, src_id);
    }
    void on_custom_query(const PublicKeyHash &src, td::BufferSlice data, td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &CatChainImpl::on_custom_query, src, std::move(data), std::move(promise));
    }
    void on_broadcast(const PublicKeyHash &src, td::BufferSlice data) override {
      td::actor::send_closure(id_, &CatChainImpl::on_broadcast, src, std::move(data));
    }
    void start() override {
      td::actor::send_closure(id_, &CatChainImpl::on_receiver_started);
    }
    explicit ChainCb(td::actor::ActorId<CatChainImpl> id) : id_(std::move(id)) {
    }

   private:
    td::actor::ActorId<CatChainImpl> id_;
  };

  auto cb = std::make_unique<ChainCb>(actor_id(this));

  receiver_ = CatChainReceiverInterface::create(
      std::move(cb), opts_, args_->keyring, args_->adnl, args_->overlay_manager, args_->ids, args_->local_id,
      args_->unique_hash, db_root_, db_suffix_, allow_unsafe_self_blocks_resync_);
  args_ = nullptr;
  //alarm_timestamp() = td::Timestamp::in(opts_.idle_timeout);
}

td::actor::ActorOwn<CatChain> CatChain::create(std::unique_ptr<Callback> callback, const CatChainOptions &opts,
                                               td::actor::ActorId<keyring::Keyring> keyring,
                                               td::actor::ActorId<adnl::Adnl> adnl,
                                               td::actor::ActorId<overlay::Overlays> overlay_manager,
                                               std::vector<CatChainNode> ids, const PublicKeyHash &local_id,
                                               const CatChainSessionId &unique_hash, std::string db_root,
                                               std::string db_suffix, bool allow_unsafe_self_blocks_resync) {
  return td::actor::create_actor<CatChainImpl>("catchain", std::move(callback), opts, std::move(keyring),
                                               std::move(adnl), std::move(overlay_manager), std::move(ids),
                                               local_id, unique_hash, std::move(db_root), std::move(db_suffix),
                                               allow_unsafe_self_blocks_resync);
}

CatChainBlock *CatChainImpl::get_block(CatChainBlockHash hash) const {
  auto it = blocks_.find(hash);
  if (it == blocks_.end()) {
    return nullptr;
  } else {
    return it->second.get();
  }
}

void CatChainImpl::destroy() {
  td::actor::send_closure(receiver_, &CatChainReceiverInterface::destroy);
  receiver_.release();
  stop();
}

}  // namespace catchain

}  // namespace ton
