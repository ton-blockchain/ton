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
#include "td/actor/PromiseFuture.h"
#include "td/utils/Random.h"
#include "td/db/RocksDb.h"
#include "td/utils/port/path.h"
#include "td/utils/overloaded.h"
#include "common/delay.h"

#include "catchain-receiver.hpp"

namespace ton {

namespace catchain {

PublicKeyHash CatChainReceiverImpl::get_source_hash(td::uint32 source_id) const {
  CHECK(source_id < sources_.size());
  return sources_[source_id]->get_hash();
}

td::uint32 CatChainReceiverImpl::add_fork() {
  return ++total_forks_;
}

void CatChainReceiverImpl::deliver_block(CatChainReceivedBlock *block) {
  VLOG(CATCHAIN_INFO) << this << ": delivering block " << block->get_hash() << " src=" << block->get_source_id()
                      << " fork=" << block->get_fork_id() << " height=" << block->get_height()
                      << " custom=" << block->is_custom();
  callback_->new_block(block->get_source_id(), block->get_fork_id(), block->get_hash(), block->get_height(),
                       block->get_height() == 1 ? CatChainBlockHash::zero() : block->get_prev_hash(),
                       block->get_dep_hashes(), block->get_deps(),
                       block->is_custom() ? block->get_payload().clone() : td::SharedSlice());

  std::vector<adnl::AdnlNodeIdShort> v;

  for (auto it : neighbours_) {
    auto S = get_source(it);
    v.push_back(S->get_adnl_id());
  }

  auto update = create_tl_object<ton_api::catchain_blockUpdate>(block->export_tl());
  auto D = serialize_tl_object(update, true, block->get_payload().as_slice());

  td::actor::send_closure(overlay_manager_, &overlay::Overlays::send_multiple_messages, std::move(v),
                          get_source(local_idx_)->get_adnl_id(), overlay_id_, std::move(D));
}

void CatChainReceiverImpl::receive_block(adnl::AdnlNodeIdShort src, tl_object_ptr<ton_api::catchain_block> block,
                                         td::BufferSlice payload) {
  auto id = CatChainReceivedBlock::block_hash(this, block, payload);
  auto B = get_block(id);
  if (B && B->initialized()) {
    return;
  }

  if (block->incarnation_ != incarnation_) {
    VLOG(CATCHAIN_WARNING) << this << ": dropping broken block from " << src << ": bad incarnation "
                           << block->incarnation_;
    return;
  }

  auto S = validate_block_sync(block, payload.as_slice());

  if (S.is_error()) {
    VLOG(CATCHAIN_WARNING) << this << ": received broken block from " << src << ": " << S.move_as_error();
    return;
  }

  if (block->src_ == static_cast<td::int32>(local_idx_)) {
    if (!allow_unsafe_self_blocks_resync_ || started_) {
      LOG(FATAL) << this << ": received unknown SELF block from " << src
                 << " (unsafe=" << allow_unsafe_self_blocks_resync_ << ")";
    } else {
      LOG(ERROR) << this << ": received unknown SELF block from " << src << ". UPDATING LOCAL DATABASE. UNSAFE";
      initial_sync_complete_at_ = td::Timestamp::in(300.0);
    }
  }

  auto raw_data = serialize_tl_object(block, true, payload.as_slice());
  create_block(std::move(block), td::SharedSlice{payload.as_slice()});

  if (!opts_.debug_disable_db) {
    db_.set(
        id, std::move(raw_data), [](td::Unit) {}, 1.0);
  }
  block_written_to_db(id);
}

void CatChainReceiverImpl::receive_block_answer(adnl::AdnlNodeIdShort src, td::BufferSlice data) {
  auto F = fetch_tl_prefix<ton_api::catchain_BlockResult>(data, true);
  if (F.is_error()) {
    VLOG(CATCHAIN_INFO) << this << ": received bad block result: " << F.move_as_error();
    return;
  }
  auto f = F.move_as_ok();
  ton_api::downcast_call(
      *f.get(),
      td::overloaded(
          [&](ton_api::catchain_blockNotFound &r) { VLOG(CATCHAIN_INFO) << this << ": catchain block not found"; },
          [&](ton_api::catchain_blockResult &r) { receive_block(src, std::move(r.block_), std::move(data)); }));
}

void CatChainReceiverImpl::receive_message_from_overlay(adnl::AdnlNodeIdShort src, td::BufferSlice data) {
  if (!read_db_) {
    return;
  }

  /*auto S = get_source_by_hash(src);
  CHECK(S != nullptr);

  if (S->blamed()) {
    VLOG(CATCHAIN_INFO) << this << ": dropping block update from blamed source " << src;
    return;
  }*/
  auto R = fetch_tl_prefix<ton_api::catchain_blockUpdate>(data, true);
  if (R.is_error()) {
    VLOG(CATCHAIN_WARNING) << this << ": dropping broken block from " << src << ": " << R.move_as_error();
    return;
  }

  auto U = R.move_as_ok();
  receive_block(src, std::move(U->block_), std::move(data));
}

void CatChainReceiverImpl::receive_broadcast_from_overlay(PublicKeyHash src, td::BufferSlice data) {
  if (!read_db_) {
    return;
  }
  callback_->on_broadcast(src, std::move(data));
}

/*void CatChainReceiverImpl::send_block(PublicKeyHash src, tl_object_ptr<ton_api::catchain_block> block,
                                      td::BufferSlice payload) {
  CHECK(read_db_);
  CHECK(src == local_id_);

  validate_block_sync(block, payload.as_slice()).ensure();
  auto B = create_block(std::move(block), td::SharedSlice{payload.as_slice()});
  CHECK(B != nullptr);

  run_scheduler();
  CHECK(B->delivered());
}*/

CatChainReceivedBlock *CatChainReceiverImpl::create_block(tl_object_ptr<ton_api::catchain_block> block,
                                                          td::SharedSlice payload) {
  if (block->height_ == 0) {
    return root_block_;
  }
  auto hash = CatChainReceivedBlock::block_hash(this, block, payload.as_slice());

  auto it = blocks_.find(hash);
  if (it != blocks_.end()) {
    if (!it->second->initialized()) {
      it->second->initialize(std::move(block), std::move(payload));
    }
    return it->second.get();
  } else {
    blocks_.emplace(hash, CatChainReceivedBlock::create(std::move(block), std::move(payload), this));
    it = blocks_.find(hash);
    return it->second.get();
  }
}

CatChainReceivedBlock *CatChainReceiverImpl::create_block(tl_object_ptr<ton_api::catchain_block_dep> block) {
  if (block->height_ == 0) {
    return root_block_;
  }
  auto hash = CatChainReceivedBlock::block_hash(this, block);
  auto it = blocks_.find(hash);
  if (it != blocks_.end()) {
    return it->second.get();
  } else {
    blocks_.emplace(hash, CatChainReceivedBlock::create(std::move(block), this));
    it = blocks_.find(hash);
    return it->second.get();
  }
}

td::Status CatChainReceiverImpl::validate_block_sync(tl_object_ptr<ton_api::catchain_block_dep> &dep) {
  TRY_STATUS_PREFIX(CatChainReceivedBlock::pre_validate_block(this, dep), "failed to validate block: ");

  if (dep->height_ > 0) {
    auto id = CatChainReceivedBlock::block_id(this, dep);
    auto B = serialize_tl_object(id, true);
    auto block = get_block(get_tl_object_sha_bits256(id));
    if (block) {
      return td::Status::OK();
    }

    auto S = get_source_by_hash(PublicKeyHash{id->src_});
    CHECK(S != nullptr);
    auto E = S->get_encryptor_sync();
    CHECK(E != nullptr);
    return E->check_signature(B.as_slice(), dep->signature_.as_slice());
  } else {
    return td::Status::OK();
  }
}

td::Status CatChainReceiverImpl::validate_block_sync(tl_object_ptr<ton_api::catchain_block> &block, td::Slice payload) {
  //LOG(INFO) << ton_api::to_string(block);
  TRY_STATUS_PREFIX(CatChainReceivedBlock::pre_validate_block(this, block, payload), "failed to validate block: ");

  if (block->height_ > 0) {
    auto id = CatChainReceivedBlock::block_id(this, block, payload);
    auto B = serialize_tl_object(id, true);

    auto S = get_source_by_hash(PublicKeyHash{id->src_});
    CHECK(S != nullptr);
    auto E = S->get_encryptor_sync();
    CHECK(E != nullptr);
    return E->check_signature(B.as_slice(), block->signature_.as_slice());
  } else {
    return td::Status::OK();
  }
}

void CatChainReceiverImpl::run_scheduler() {
  while (!to_run_.empty()) {
    auto B = to_run_.front();
    to_run_.pop_front();

    B->run();
  }
}

void CatChainReceiverImpl::run_block(CatChainReceivedBlock *block) {
  to_run_.push_back(block);
}

CatChainReceivedBlock *CatChainReceiverImpl::get_block(CatChainBlockHash hash) const {
  auto it = blocks_.find(hash);
  if (it == blocks_.end()) {
    return nullptr;
  } else {
    return it->second.get();
  }
}

void CatChainReceiverImpl::add_block_cont_3(tl_object_ptr<ton_api::catchain_block> block, td::BufferSlice payload) {
  last_sent_block_ = create_block(std::move(block), td::SharedSlice{payload.as_slice()});
  last_sent_block_->written();

  run_scheduler();
  if (!intentional_fork_) {
    CHECK(last_sent_block_->delivered());
  }

  active_send_ = false;
  if (pending_blocks_.size() > 0) {
    auto B = std::move(pending_blocks_.front());
    pending_blocks_.pop_front();
    add_block(std::move(B->payload_), std::move(B->deps_));
  }
}

void CatChainReceiverImpl::add_block_cont_2(tl_object_ptr<ton_api::catchain_block> block, td::BufferSlice payload) {
  if (opts_.debug_disable_db) {
    add_block_cont_3(std::move(block), std::move(payload));
    return;
  }

  auto id = CatChainReceivedBlock::block_hash(this, block, payload);

  td::BufferSlice raw_data{32};
  raw_data.as_slice().copy_from(as_slice(id));

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block = std::move(block),
                                       payload = std::move(payload)](td::Result<td::Unit> R) mutable {
    R.ensure();
    td::actor::send_closure(SelfId, &CatChainReceiverImpl::add_block_cont_3, std::move(block), std::move(payload));
  });

  db_.set(CatChainBlockHash::zero(), std::move(raw_data), std::move(P), 0);
}

void CatChainReceiverImpl::add_block_cont(tl_object_ptr<ton_api::catchain_block> block, td::BufferSlice payload) {
  validate_block_sync(block, payload.as_slice()).ensure();
  if (opts_.debug_disable_db) {
    add_block_cont_2(std::move(block), std::move(payload));
    return;
  }
  auto id = CatChainReceivedBlock::block_hash(this, block, payload.as_slice());

  auto raw_data = serialize_tl_object(block, true, payload.as_slice());

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block = std::move(block),
                                       payload = std::move(payload)](td::Result<td::Unit> R) mutable {
    R.ensure();
    td::actor::send_closure(SelfId, &CatChainReceiverImpl::add_block_cont_2, std::move(block), std::move(payload));
  });

  db_.set(id, std::move(raw_data), std::move(P), 0);
}

void CatChainReceiverImpl::add_block(td::BufferSlice payload, std::vector<CatChainBlockHash> deps) {
  if (active_send_) {
    auto B = std::make_unique<PendingBlock>(std::move(payload), std::move(deps));
    pending_blocks_.push_back(std::move(B));
    return;
  }
  active_send_ = true;

  auto S = get_source_by_hash(local_id_);
  CHECK(S != nullptr);
  CHECK(S->get_id() == local_idx_);
  if (!intentional_fork_) {
    CHECK(!S->blamed());
  }

  auto prev = last_sent_block_->export_tl_dep();

  std::vector<tl_object_ptr<ton_api::catchain_block_dep>> deps_arr;
  deps_arr.resize(deps.size());
  for (size_t i = 0; i < deps.size(); i++) {
    auto B = get_block(deps[i]);
    LOG_CHECK(B != nullptr) << this << ": cannot find block with hash " << deps[i];
    if (!intentional_fork_) {
      CHECK(B->get_source_id() != local_idx_);
    }
    deps_arr[i] = B->export_tl_dep();
  }

  auto height = prev->height_ + 1;
  auto block_data = create_tl_object<ton_api::catchain_block_data>(std::move(prev), std::move(deps_arr));
  auto block = create_tl_object<ton_api::catchain_block>(incarnation_, local_idx_, height, std::move(block_data),
                                                         td::BufferSlice());

  auto id = CatChainReceivedBlock::block_id(this, block, payload);
  auto id_s = serialize_tl_object(id, true);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), print_id = print_id(), block = std::move(block),
                                       payload = std::move(payload)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      LOG(FATAL) << print_id << ": failed to sign: " << R.move_as_error();
      return;
    }
    block->signature_ = R.move_as_ok();
    td::actor::send_closure(SelfId, &CatChainReceiverImpl::add_block_cont, std::move(block), std::move(payload));
  });

  td::actor::send_closure_later(keyring_, &keyring::Keyring::sign_message, local_id_, std::move(id_s), std::move(P));
}

void CatChainReceiverImpl::debug_add_fork_cont(tl_object_ptr<ton_api::catchain_block> block, td::BufferSlice payload) {
  validate_block_sync(block, payload.as_slice()).ensure();
  auto B = create_block(std::move(block), td::SharedSlice{payload.as_slice()});
  B->written();

  run_scheduler();
  CHECK(B->delivered());

  active_send_ = false;
  if (pending_blocks_.size() > 0) {
    auto B = std::move(pending_blocks_.front());
    pending_blocks_.pop_front();
    add_block(std::move(B->payload_), std::move(B->deps_));
  }
}

void CatChainReceiverImpl::debug_add_fork(td::BufferSlice payload, CatChainBlockHeight height,
                                          std::vector<CatChainBlockHash> deps) {
  intentional_fork_ = true;
  auto S = get_source_by_hash(local_id_);
  CHECK(S != nullptr);
  CHECK(S->get_id() == local_idx_);

  if (height > S->received_height() + 1) {
    height = S->received_height() + 1;
  }

  CHECK(height > 0);
  CatChainReceivedBlock *prev;
  if (height == 1) {
    prev = root_block_;
  } else {
    prev = sources_[local_idx_]->get_block(height - 1);
    CHECK(prev);
  }

  std::vector<tl_object_ptr<ton_api::catchain_block_dep>> deps_arr;
  deps_arr.resize(deps.size());
  for (size_t i = 0; i < deps.size(); i++) {
    auto B = get_block(deps[i]);
    LOG_CHECK(B != nullptr) << this << ": cannot find block with hash " << deps[i];
    CHECK(B->get_source_id() != local_idx_);
    deps_arr[i] = B->export_tl_dep();
  }

  auto block_data = create_tl_object<ton_api::catchain_block_data>(prev->export_tl_dep(), std::move(deps_arr));
  auto block = create_tl_object<ton_api::catchain_block>(incarnation_, local_idx_, height, std::move(block_data),
                                                         td::BufferSlice());

  auto id = CatChainReceivedBlock::block_id(this, block, payload);
  auto id_s = serialize_tl_object(id, true);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), print_id = print_id(), block = std::move(block),
                                       payload = std::move(payload)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      LOG(FATAL) << print_id << ": failed to sign: " << R.move_as_error();
      return;
    }
    block->signature_ = R.move_as_ok();
    td::actor::send_closure(SelfId, &CatChainReceiverImpl::debug_add_fork_cont, std::move(block), std::move(payload));
  });

  td::actor::send_closure_later(keyring_, &keyring::Keyring::sign_message, local_id_, std::move(id_s), std::move(P));
}

CatChainReceiverImpl::CatChainReceiverImpl(std::unique_ptr<Callback> callback, CatChainOptions opts,
                                           td::actor::ActorId<keyring::Keyring> keyring,
                                           td::actor::ActorId<adnl::Adnl> adnl,
                                           td::actor::ActorId<overlay::Overlays> overlay_manager,
                                           std::vector<CatChainNode> ids, PublicKeyHash local_id,
                                           CatChainSessionId unique_hash, std::string db_root, std::string db_suffix,
                                           bool allow_unsafe_self_blocks_resync)
    : callback_(std::move(callback))
    , opts_(std::move(opts))
    , keyring_(keyring)
    , adnl_(adnl)
    , overlay_manager_(overlay_manager)
    , local_id_(local_id)
    , db_root_(db_root)
    , db_suffix_(db_suffix)
    , allow_unsafe_self_blocks_resync_(allow_unsafe_self_blocks_resync) {
  std::vector<td::Bits256> short_ids;
  local_idx_ = static_cast<td::uint32>(ids.size());
  for (auto &id : ids) {
    td::uint32 seq = static_cast<td::uint32>(sources_.size());
    auto R = CatChainReceiverSource::create(this, id.pub_key, id.adnl_id, seq);
    auto S = R.move_as_ok();
    auto h = id.pub_key.compute_short_id();
    short_ids.push_back(h.bits256_value());
    sources_hashes_[h] = seq;
    sources_adnl_addrs_[id.adnl_id] = seq;
    sources_.push_back(std::move(S));

    if (h == local_id_) {
      CHECK(local_idx_ == static_cast<td::uint32>(ids.size()));
      local_idx_ = seq;
    }
  }
  CHECK(local_idx_ != static_cast<td::uint32>(ids.size()));

  //std::sort(short_ids.begin(), short_ids.end());
  auto F = create_tl_object<ton_api::catchain_firstblock>(unique_hash, std::move(short_ids));

  overlay_full_id_ = overlay::OverlayIdFull{serialize_tl_object(F, true)};
  overlay_id_ = overlay_full_id_.compute_short_id();
  incarnation_ = overlay_id_.bits256_value();

  auto R = CatChainReceivedBlock::create_root(get_sources_cnt(), incarnation_, this);
  root_block_ = R.get();
  blocks_[root_block_->get_hash()] = std::move(R);
  last_sent_block_ = root_block_;

  choose_neighbours();
}

void CatChainReceiverImpl::start_up() {
  std::vector<adnl::AdnlNodeIdShort> ids;
  for (td::uint32 i = 0; i < get_sources_cnt(); i++) {
    ids.push_back(get_source(i)->get_adnl_id());
  }
  std::map<PublicKeyHash, td::uint32> root_keys;
  for (td::uint32 i = 0; i < get_sources_cnt(); i++) {
    root_keys.emplace(get_source(i)->get_hash(), 16 << 20);
  }
  td::actor::send_closure(overlay_manager_, &overlay::Overlays::create_private_overlay,
                          get_source(local_idx_)->get_adnl_id(), overlay_full_id_.clone(), std::move(ids),
                          make_callback(), overlay::OverlayPrivacyRules{0, 0, std::move(root_keys)});

  CHECK(root_block_);

  if (!opts_.debug_disable_db) {
    std::shared_ptr<td::KeyValue> kv = std::make_shared<td::RocksDb>(
        td::RocksDb::open(db_root_ + "/catchainreceiver" + db_suffix_ + td::base64url_encode(as_slice(incarnation_)))
            .move_as_ok());
    db_ = DbType{std::move(kv)};

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<DbType::GetResult> R) {
      R.ensure();
      auto g = R.move_as_ok();
      if (g.status == td::KeyValue::GetStatus::NotFound) {
        td::actor::send_closure(SelfId, &CatChainReceiverImpl::read_db);
      } else {
        auto B = std::move(g.value);
        CHECK(B.size() == 32);
        CatChainBlockHash x;
        as_slice(x).copy_from(B.as_slice());
        td::actor::send_closure(SelfId, &CatChainReceiverImpl::read_db_from, x);
      }
    });

    db_.get(CatChainBlockHash::zero(), std::move(P));
  } else {
    read_db();
  }
}

void CatChainReceiverImpl::tear_down() {
  td::actor::send_closure(overlay_manager_, &overlay::Overlays::delete_overlay, get_source(local_idx_)->get_adnl_id(),
                          overlay_id_);
}

void CatChainReceiverImpl::read_db_from(CatChainBlockHash id) {
  pending_in_db_ = 1;
  db_root_block_ = id;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id](td::Result<DbType::GetResult> R) {
    R.ensure();
    auto g = R.move_as_ok();
    CHECK(g.status == td::KeyValue::GetStatus::Ok);

    td::actor::send_closure(SelfId, &CatChainReceiverImpl::read_block_from_db, id, std::move(g.value));
  });

  db_.get(id, std::move(P));
}

void CatChainReceiverImpl::read_block_from_db(CatChainBlockHash id, td::BufferSlice data) {
  pending_in_db_--;

  auto F = fetch_tl_prefix<ton_api::catchain_block>(data, true);
  F.ensure();

  auto block = F.move_as_ok();
  auto payload = std::move(data);

  auto block_id = CatChainReceivedBlock::block_hash(this, block, payload);
  CHECK(block_id == id);

  auto B = get_block(id);
  if (B && B->initialized()) {
    CHECK(B->in_db());
    if (!pending_in_db_) {
      read_db();
    }
    return;
  }

  auto source = get_source(block->src_);
  CHECK(source != nullptr);

  CHECK(block->incarnation_ == incarnation_);

  validate_block_sync(block, payload).ensure();

  B = create_block(std::move(block), td::SharedSlice{payload.as_slice()});
  B->written();
  CHECK(B);

  auto deps = B->get_dep_hashes();
  deps.push_back(B->get_prev_hash());
  for (auto &dep : deps) {
    auto dep_block = get_block(dep);
    if (!dep_block || !dep_block->initialized()) {
      pending_in_db_++;
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), dep](td::Result<DbType::GetResult> R) {
        R.ensure();
        auto g = R.move_as_ok();
        CHECK(g.status == td::KeyValue::GetStatus::Ok);

        td::actor::send_closure(SelfId, &CatChainReceiverImpl::read_block_from_db, dep, std::move(g.value));
      });

      db_.get(dep, std::move(P));
    }
  }

  if (!pending_in_db_) {
    read_db();
  }
}

void CatChainReceiverImpl::read_db() {
  if (!db_root_block_.is_zero()) {
    run_scheduler();
    last_sent_block_ = get_block(db_root_block_);
    CHECK(last_sent_block_);
    CHECK(last_sent_block_->delivered());
  }

  read_db_ = true;

  next_rotate_ = td::Timestamp::in(60 + td::Random::fast(0, 60));
  next_sync_ = td::Timestamp::in(0.001 * td::Random::fast(0, 60));
  initial_sync_complete_at_ = td::Timestamp::in(allow_unsafe_self_blocks_resync_ ? 300.0 : 5.0);
  alarm_timestamp().relax(next_rotate_);
  alarm_timestamp().relax(next_sync_);
  alarm_timestamp().relax(initial_sync_complete_at_);
}

td::actor::ActorOwn<CatChainReceiverInterface> CatChainReceiverInterface::create(
    std::unique_ptr<Callback> callback, CatChainOptions opts, td::actor::ActorId<keyring::Keyring> keyring,
    td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<overlay::Overlays> overlay_manager,
    std::vector<CatChainNode> ids, PublicKeyHash local_id, CatChainSessionId unique_hash, std::string db_root,
    std::string db_suffix, bool allow_unsafe_self_blocks_resync) {
  auto A = td::actor::create_actor<CatChainReceiverImpl>(
      "catchainreceiver", std::move(callback), std::move(opts), keyring, adnl, overlay_manager, std::move(ids),
      local_id, unique_hash, db_root, db_suffix, allow_unsafe_self_blocks_resync);
  return std::move(A);
}

CatChainReceiverSource *CatChainReceiverImpl::get_source_by_hash(PublicKeyHash source_hash) const {
  auto it = sources_hashes_.find(source_hash);
  if (it == sources_hashes_.end()) {
    return nullptr;
  }
  return get_source(it->second);
}

CatChainReceiverSource *CatChainReceiverImpl::get_source_by_adnl_id(adnl::AdnlNodeIdShort source_hash) const {
  auto it = sources_adnl_addrs_.find(source_hash);
  if (it == sources_adnl_addrs_.end()) {
    return nullptr;
  }
  return get_source(it->second);
}

void CatChainReceiverImpl::receive_query_from_overlay(adnl::AdnlNodeIdShort src, td::BufferSlice data,
                                                      td::Promise<td::BufferSlice> promise) {
  if (!read_db_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "db not read"));
    return;
  }
  td::PerfWarningTimer t{"catchain query process", 0.05};
  auto F = fetch_tl_object<ton_api::Function>(data.clone(), true);
  if (F.is_error()) {
    callback_->on_custom_query(get_source_by_adnl_id(src)->get_hash(), std::move(data), std::move(promise));
    //LOG(WARNING) << this << ": unknown query from " << src;
    return;
  }
  auto f = F.move_as_ok();
  ton_api::downcast_call(*f.get(), [&](auto &obj) { this->process_query(src, obj, std::move(promise)); });
}

void CatChainReceiverImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::catchain_getBlock &query,
                                         td::Promise<td::BufferSlice> promise) {
  auto it = blocks_.find(query.block_);
  if (it == blocks_.end() || it->second->get_height() == 0 || !it->second->initialized()) {
    promise.set_value(serialize_tl_object(create_tl_object<ton_api::catchain_blockNotFound>(), true));
  } else {
    promise.set_value(serialize_tl_object(create_tl_object<ton_api::catchain_blockResult>(it->second->export_tl()),
                                          true, it->second->get_payload().as_slice()));
  }
}

void CatChainReceiverImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::catchain_getBlocks &query,
                                         td::Promise<td::BufferSlice> promise) {
  if (query.blocks_.size() > 100) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "too many blocks"));
    return;
  }
  td::int32 cnt = 0;
  for (auto &b : query.blocks_) {
    auto it = blocks_.find(b);
    if (it != blocks_.end() && it->second->get_height() > 0) {
      auto block = create_tl_object<ton_api::catchain_blockUpdate>(it->second->export_tl());
      CHECK(it->second->get_payload().size() > 0);
      auto B = serialize_tl_object(block, true, it->second->get_payload().clone());
      td::actor::send_closure(overlay_manager_, &overlay::Overlays::send_message, src,
                              get_source(local_idx_)->get_adnl_id(), overlay_id_, std::move(B));
      cnt++;
    }
  }
  promise.set_value(serialize_tl_object(create_tl_object<ton_api::catchain_sent>(cnt), true));
}

void CatChainReceiverImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::catchain_getBlockHistory &query,
                                         td::Promise<td::BufferSlice> promise) {
  auto h = query.height_;
  if (h <= 0) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "not-positive height"));
    return;
  }
  if (h > 100) {
    h = 100;
  }
  std::set<CatChainBlockHash> s{query.stop_if_.begin(), query.stop_if_.end()};

  auto B = get_block(query.block_);
  if (B == nullptr) {
    promise.set_value(serialize_tl_object(create_tl_object<ton_api::catchain_sent>(0), true));
    return;
  }
  if (static_cast<CatChainBlockHeight>(h) > B->get_height()) {
    h = B->get_height();
  }
  td::uint32 cnt = 0;
  while (h-- > 0) {
    if (s.find(B->get_hash()) != s.end()) {
      break;
    }
    auto block = create_tl_object<ton_api::catchain_blockUpdate>(B->export_tl());
    CHECK(B->get_payload().size() > 0);
    auto BB = serialize_tl_object(block, true, B->get_payload().as_slice());
    td::actor::send_closure(overlay_manager_, &overlay::Overlays::send_message, src,
                            get_source(local_idx_)->get_adnl_id(), overlay_id_, std::move(BB));
    B = B->get_prev();
    cnt++;
  }
  promise.set_value(serialize_tl_object(create_tl_object<ton_api::catchain_sent>(cnt), true));
}

void CatChainReceiverImpl::process_query(adnl::AdnlNodeIdShort src, ton_api::catchain_getDifference &query,
                                         td::Promise<td::BufferSlice> promise) {
  auto &vt = query.rt_;
  if (vt.size() != get_sources_cnt()) {
    VLOG(CATCHAIN_WARNING) << this << ": incorrect query from " << src;
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "bad vt size"));
    return;
  }
  for (td::uint32 i = 0; i < get_sources_cnt(); i++) {
    if (vt[i] >= 0) {
      auto S = get_source(i);
      if (S->fork_is_found()) {
        auto obj = fetch_tl_object<ton_api::catchain_block_data_fork>(S->fork_proof(), true);
        obj.ensure();
        auto f = obj.move_as_ok();
        promise.set_value(
            create_serialize_tl_object<ton_api::catchain_differenceFork>(std::move(f->left_), std::move(f->right_)));
        return;
      }
    }
  }

  std::vector<td::int32> my_vt(get_sources_cnt());
  td::uint64 total = 0;
  for (td::uint32 i = 0; i < get_sources_cnt(); i++) {
    if (vt[i] >= 0) {
      auto x = static_cast<CatChainBlockHeight>(vt[i]);
      auto S = get_source(i);
      if (S->delivered_height() > x) {
        total += S->delivered_height() - x;
      }
      my_vt[i] = S->delivered_height();
    } else {
      my_vt[i] = -1;
    }
  }

  const td::uint32 max_send = 100;

  td::int32 l = 0;
  td::int32 r = max_send + 1;
  while (r - l > 1) {
    td::int32 x = (r + l) / 2;
    td::uint64 sum = 0;
    for (td::uint32 i = 0; i < get_sources_cnt(); i++) {
      if (vt[i] >= 0 && my_vt[i] > vt[i]) {
        sum += (my_vt[i] - vt[i] > x) ? x : (my_vt[i] - vt[i]);
      }
    }
    if (sum > max_send) {
      r = x;
    } else {
      l = x;
    }
  }
  CHECK(r > 0);
  for (td::uint32 i = 0; i < get_sources_cnt(); i++) {
    if (vt[i] >= 0 && my_vt[i] > vt[i]) {
      auto S = get_source(i);
      auto t = (my_vt[i] - vt[i] > r) ? r : (my_vt[i] - vt[i]);
      CHECK(t > 0);
      while (t-- > 0) {
        auto M = S->get_block(++vt[i]);
        CHECK(M != nullptr);
        auto block = create_tl_object<ton_api::catchain_blockUpdate>(M->export_tl());
        CHECK(M->get_payload().size() > 0);
        auto BB = serialize_tl_object(block, true, M->get_payload().as_slice());
        td::actor::send_closure(overlay_manager_, &overlay::Overlays::send_message, src,
                                get_source(local_idx_)->get_adnl_id(), overlay_id_, std::move(BB));
      }
    }
  }

  promise.set_value(serialize_tl_object(create_tl_object<ton_api::catchain_difference>(std::move(vt)), true));
}

void CatChainReceiverImpl::got_fork_proof(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::catchain_differenceFork>(std::move(data), true);
  if (F.is_error()) {
    VLOG(CATCHAIN_WARNING) << this << ": received bad fork proof: " << F.move_as_error();
    return;
  }
  auto f = F.move_as_ok();
  {
    td::Status S;
    S = validate_block_sync(f->left_);
    if (S.is_error()) {
      VLOG(CATCHAIN_WARNING) << this << ": incorrect fork blame: left is invalid: " << S.move_as_error();
      return;
    }
    S = validate_block_sync(f->right_);
    if (S.is_error()) {
      VLOG(CATCHAIN_WARNING) << this << ": incorrect fork blame: right is invalid: " << S.move_as_error();
      return;
    }
  }

  // block is incorrect, since blocks are
  if (f->left_->height_ != f->right_->height_ || f->left_->src_ != f->right_->src_ ||
      f->left_->data_hash_ == f->right_->data_hash_) {
    VLOG(CATCHAIN_WARNING) << this << ": incorrect fork blame: not a fork";
    return;
  }

  auto S = get_source(f->left_->src_);
  S->on_found_fork_proof(
      create_serialize_tl_object<ton_api::catchain_block_data_fork>(std::move(f->left_), std::move(f->right_)));
  S->blame();
}

void CatChainReceiverImpl::synchronize_with(CatChainReceiverSource *S) {
  CHECK(!S->blamed());
  std::vector<td::int32> rt(get_sources_cnt());
  for (td::uint32 i = 0; i < get_sources_cnt(); i++) {
    auto SS = get_source(i);
    if (SS->blamed()) {
      rt[i] = -1;
    } else {
      rt[i] = S->delivered_height();
    }
  }

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), src = S->get_hash(), print_id = print_id()](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          VLOG(CATCHAIN_INFO) << print_id << ": timedout syncronize query to " << src;
          return;
        }
        auto data = R.move_as_ok();
        auto X = fetch_tl_object<ton_api::catchain_Difference>(data.clone(), true);
        if (X.is_error()) {
          VLOG(CATCHAIN_WARNING) << print_id << ": received incorrect answer to syncronize query from " << src << ": "
                                 << X.move_as_error();
          return;
        }
        auto A = X.move_as_ok();

        if (A->get_id() == ton_api::catchain_differenceFork::ID) {
          td::actor::send_closure(SelfId, &CatChainReceiverImpl::got_fork_proof, std::move(data));
        }
        // use answer ?
        return;
      });
  td::actor::send_closure(overlay_manager_, &overlay::Overlays::send_query, S->get_adnl_id(),
                          get_source(local_idx_)->get_adnl_id(), overlay_id_, "sync", std::move(P),
                          td::Timestamp::in(5.0),
                          serialize_tl_object(create_tl_object<ton_api::catchain_getDifference>(std::move(rt)), true));

  if (S->delivered_height() < S->received_height()) {
    auto B = S->get_block(S->delivered_height() + 1);
    CHECK(B->initialized());

    std::vector<CatChainBlockHash> vec;
    B->find_pending_deps(vec, 16);

    for (auto &hash : vec) {
      auto P = td::PromiseCreator::lambda(
          [SelfId = actor_id(this), print_id = print_id(), src = S->get_adnl_id()](td::Result<td::BufferSlice> R) {
            if (R.is_error()) {
              VLOG(CATCHAIN_INFO) << print_id << ": timedout syncronize query to " << src;
            } else {
              td::actor::send_closure(SelfId, &CatChainReceiverImpl::receive_block_answer, src, R.move_as_ok());
            }
          });
      auto query = serialize_tl_object(create_tl_object<ton_api::catchain_getBlock>(hash), true);
      td::actor::send_closure(overlay_manager_, &overlay::Overlays::send_query, S->get_adnl_id(),
                              get_source(local_idx_)->get_adnl_id(), overlay_id_, "sync blocks", std::move(P),
                              td::Timestamp::in(2.0), std::move(query));
    }
  }
}

void CatChainReceiverImpl::choose_neighbours() {
  std::vector<td::uint32> n;
  n.resize(get_max_neighbours());

  td::uint32 size = 0;
  for (td::uint32 i = 0; i < get_sources_cnt(); i++) {
    if (i == local_idx_) {
      continue;
    }
    auto S = get_source(i);
    if (!S->blamed()) {
      size++;
      if (size <= n.size()) {
        n[size - 1] = i;
      } else {
        td::uint32 id = td::Random::fast(0, size - 1);
        if (id < n.size()) {
          n[id] = i;
        }
      }
    }
  }
  if (size < n.size()) {
    n.resize(size);
  }
  neighbours_ = std::move(n);
}

bool CatChainReceiverImpl::unsafe_start_up_check_completed() {
  auto S = get_source(local_idx_);
  CHECK(!S->blamed());
  if (S->has_unreceived() || S->has_undelivered()) {
    LOG(INFO) << "catchain: has_unreceived=" << S->has_unreceived() << " has_undelivered=" << S->has_undelivered();
    run_scheduler();
    initial_sync_complete_at_ = td::Timestamp::in(60.0);
    return false;
  }
  auto h = S->delivered_height();
  if (h == 0) {
    CHECK(last_sent_block_->get_height() == 0);
    CHECK(!unsafe_root_block_writing_);
    return true;
  }
  if (last_sent_block_->get_height() == h) {
    CHECK(!unsafe_root_block_writing_);
    return true;
  }
  if (unsafe_root_block_writing_) {
    initial_sync_complete_at_ = td::Timestamp::in(5.0);
    LOG(INFO) << "catchain: writing=true";
    return false;
  }

  unsafe_root_block_writing_ = true;
  auto B = S->get_block(h);
  CHECK(B != nullptr);
  CHECK(B->delivered());
  CHECK(B->in_db());

  auto id = B->get_hash();

  td::BufferSlice raw_data{32};
  raw_data.as_slice().copy_from(as_slice(id));

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block = B](td::Result<td::Unit> R) mutable {
    R.ensure();
    td::actor::send_closure(SelfId, &CatChainReceiverImpl::written_unsafe_root_block, block);
  });

  db_.set(CatChainBlockHash::zero(), std::move(raw_data), std::move(P), 0);
  initial_sync_complete_at_ = td::Timestamp::in(5.0);
  LOG(INFO) << "catchain: need update root";
  return false;
}

void CatChainReceiverImpl::written_unsafe_root_block(CatChainReceivedBlock *block) {
  CHECK(last_sent_block_->get_height() < block->get_height());
  last_sent_block_ = block;
  unsafe_root_block_writing_ = false;
}

void CatChainReceiverImpl::alarm() {
  alarm_timestamp() = td::Timestamp::never();
  if (next_sync_ && next_sync_.is_in_past()) {
    next_sync_ = td::Timestamp::in(td::Random::fast(0.1, 0.2));
    for (auto i = 0; i < 3; i++) {
      auto S = get_source(td::Random::fast(0, get_sources_cnt() - 1));
      CHECK(S != nullptr);
      if (!S->blamed()) {
        synchronize_with(S);
        break;
      }
    }
  }
  if (next_rotate_ && next_rotate_.is_in_past()) {
    next_rotate_ = td::Timestamp::in(td::Random::fast(60.0, 120.0));
    choose_neighbours();
  }
  if (!started_ && read_db_ && initial_sync_complete_at_ && initial_sync_complete_at_.is_in_past()) {
    bool allow = false;
    if (allow_unsafe_self_blocks_resync_) {
      allow = unsafe_start_up_check_completed();
    } else {
      allow = true;
    }
    if (allow) {
      initial_sync_complete_at_ = td::Timestamp::never();
      started_ = true;
      callback_->start();
    }
  }
  alarm_timestamp().relax(next_rotate_);
  alarm_timestamp().relax(next_sync_);
  alarm_timestamp().relax(initial_sync_complete_at_);
}

void CatChainReceiverImpl::send_fec_broadcast(td::BufferSlice data) {
  td::actor::send_closure(overlay_manager_, &overlay::Overlays::send_broadcast_fec_ex,
                          get_source(local_idx_)->get_adnl_id(), overlay_id_, local_id_, 0, std::move(data));
}
void CatChainReceiverImpl::send_custom_query_data(PublicKeyHash dst, std::string name,
                                                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                                                  td::BufferSlice query) {
  auto S = get_source_by_hash(dst);
  td::actor::send_closure(overlay_manager_, &overlay::Overlays::send_query, S->get_adnl_id(),
                          get_source(local_idx_)->get_adnl_id(), overlay_id_, std::move(name), std::move(promise),
                          timeout, std::move(query));
}

void CatChainReceiverImpl::send_custom_query_data_via(PublicKeyHash dst, std::string name,
                                                      td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                                                      td::BufferSlice query, td::uint64 max_answer_size,
                                                      td::actor::ActorId<adnl::AdnlSenderInterface> via) {
  auto S = get_source_by_hash(dst);
  td::actor::send_closure(overlay_manager_, &overlay::Overlays::send_query_via, S->get_adnl_id(),
                          get_source(local_idx_)->get_adnl_id(), overlay_id_, std::move(name), std::move(promise),
                          timeout, std::move(query), max_answer_size, via);
}

void CatChainReceiverImpl::send_custom_message_data(PublicKeyHash dst, td::BufferSlice data) {
  auto S = get_source_by_hash(dst);
  td::actor::send_closure(overlay_manager_, &overlay::Overlays::send_message, S->get_adnl_id(),
                          get_source(local_idx_)->get_adnl_id(), overlay_id_, std::move(data));
}

void CatChainReceiverImpl::block_written_to_db(CatChainBlockHash hash) {
  auto block = get_block(hash);
  CHECK(block);

  block->written();
  run_scheduler();
}

static void destroy_db(std::string name, td::uint32 attempt) {
  auto S = td::RocksDb::destroy(name);
  if (S.is_ok()) {
    return;
  }
  if (S.is_error() && attempt >= 10) {
    LOG(ERROR) << "failed to destroy catchain " << name << ": " << S;
  } else {
    LOG(DEBUG) << "failed to destroy catchain " << name << ": " << S;
    delay_action([name, attempt]() { destroy_db(name, attempt); }, td::Timestamp::in(1.0));
  }
}

void CatChainReceiverImpl::destroy() {
  auto name = db_root_ + "/catchainreceiver" + db_suffix_ + td::base64url_encode(as_slice(incarnation_));
  delay_action([name]() { destroy_db(name, 0); }, td::Timestamp::in(1.0));
  stop();
}

}  // namespace catchain

}  // namespace ton
