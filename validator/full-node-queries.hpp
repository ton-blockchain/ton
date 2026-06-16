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

#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api_id_names.h"
#include "td/utils/tl_helpers.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/ton-io.hpp"
#include "ton/ton-tl.hpp"
#include "validator/validator.h"

#include "full-node-serializer.hpp"
#include "full-node.h"

namespace ton {

namespace validator {

namespace fullnode {

class BlockFullSender : public td::actor::Actor {
 public:
  BlockFullSender(BlockIdExt block_id, bool next, td::actor::ActorId<ValidatorManagerInterface> manager,
                  td::Promise<td::BufferSlice> promise)
      : block_id_(block_id), next_(next), manager_(manager), promise_(std::move(promise)) {
  }
  void abort_query(td::Status error) {
    promise_.set_value(create_serialize_tl_object<ton_api::tonNode_dataFullEmpty>());
    stop();
  }
  void finish_query() {
    promise_.set_result(
        serialize_block_full(block_id_, proof_, data_, is_proof_link_, false));  // compression_enabled = false
    stop();
  }
  void start_up() override {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &BlockFullSender::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &BlockFullSender::got_block_handle, R.move_as_ok());
      }
    });
    td::actor::send_closure(manager_, &ValidatorManagerInterface::get_block_handle, block_id_, false, std::move(P));
  }

  void got_block_handle(BlockHandle handle) {
    if (next_) {
      if (!handle->inited_next_left()) {
        return abort_query(td::Status::Error(ErrorCode::notready, "next not known"));
      }
      next_ = false;
      block_id_ = handle->one_next(true);
      start_up();
      return;
    }
    if (!handle->received() || (!handle->inited_proof() && !handle->inited_proof_link()) || handle->deleted()) {
      return abort_query(td::Status::Error(ErrorCode::notready, "not in db"));
    }
    handle_ = std::move(handle);
    is_proof_link_ = !handle_->inited_proof();

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<BlockData>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &BlockFullSender::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &BlockFullSender::got_block_data, R.move_as_ok()->data());
      }
    });
    td::actor::send_closure(manager_, &ValidatorManagerInterface::get_block_data_from_db, handle_, std::move(P));

    if (!is_proof_link_) {
      auto Q = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<Proof>> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &BlockFullSender::abort_query, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &BlockFullSender::got_block_proof, R.move_as_ok()->data());
        }
      });
      td::actor::send_closure(manager_, &ValidatorManagerInterface::get_block_proof_from_db, handle_, std::move(Q));
    } else {
      auto Q = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ProofLink>> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &BlockFullSender::abort_query, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &BlockFullSender::got_block_proof, R.move_as_ok()->data());
        }
      });
      td::actor::send_closure(manager_, &ValidatorManagerInterface::get_block_proof_link_from_db, handle_,
                              std::move(Q));
    }
  }

  void got_block_data(td::BufferSlice data) {
    data_ = std::move(data);
    if (!proof_.empty()) {
      finish_query();
    }
  }

  void got_block_proof(td::BufferSlice data) {
    proof_ = std::move(data);
    if (!data_.empty()) {
      finish_query();
    }
  }

 private:
  BlockIdExt block_id_;
  bool next_;
  BlockHandle handle_;
  bool is_proof_link_;
  td::BufferSlice proof_;
  td::BufferSlice data_;
  td::actor::ActorId<ValidatorManagerInterface> manager_;
  td::Promise<td::BufferSlice> promise_;
};

class NextBlocksFullSender : public td::actor::Actor {
 public:
  NextBlocksFullSender(BlockIdExt prev_id, td::uint32 max_blocks, td::actor::ActorId<ValidatorManagerInterface> manager,
                       td::Promise<td::BufferSlice> promise)
      : prev_id_(prev_id), max_blocks_(max_blocks), manager_(manager), promise_(std::move(promise)) {
  }
  void start_up() override {
    [](NextBlocksFullSender *self) -> td::actor::Task<> {
      auto R = co_await self->run().start().wrap();
      if (R.is_error()) {
        VLOG(full_node, DEBUG) << "NextBlocksFullSender error (sending " << self->result_.size()
                               << " blocks): " << R.move_as_error();
      } else {
        VLOG(full_node, DEBUG) << "NextBlocksFullSender OK (sending " << self->result_.size() << " blocks)";
      }
      self->promise_.set_value(create_serialize_tl_object<ton_api::tonNode_nextBlocksFull>(std::move(self->result_)));
      self->stop();
      co_return {};
    }(this)
                                          .start()
                                          .detach();
  }

  td::actor::Task<> run() {
    max_blocks_ = std::min(max_blocks_, MAX_BLOCKS);
    if (!prev_id_.is_masterchain_ext()) {
      co_return td::Status::Error("expected masterchain block");
    }
    auto handle = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_block_handle, prev_id_, false);
    size_t total_size = 0;
    while (result_.size() < max_blocks_ && handle->inited_next()) {
      handle =
          co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_block_handle, handle->one_next(true), true);
      if (!handle->received() || !handle->inited_proof() || handle->deleted()) {
        break;
      }
      auto block_task = td::actor::ask(manager_, &ValidatorManagerInterface::get_block_data_from_db, handle);
      auto proof_task = td::actor::ask(manager_, &ValidatorManagerInterface::get_block_proof_from_db, handle);
      auto block = co_await std::move(block_task);
      auto proof = co_await std::move(proof_task);
      auto obj = CO_TRY(serialize_block_full_obj(block->block_id(), proof->data(), block->data(), false,
                                                 /* compression_enabled = */ true));
      size_t serialized_size = td::tl_calc_length(*obj);
      if (total_size + serialized_size > MAX_TOTAL_SIZE && !result_.empty()) {
        break;
      }
      total_size += serialized_size;
      result_.push_back(std::move(obj));
      if (total_size > MAX_TOTAL_SIZE) {
        break;
      }
    }
    co_return {};
  }

 private:
  BlockIdExt prev_id_;
  td::uint32 max_blocks_;
  td::actor::ActorId<ValidatorManagerInterface> manager_;
  td::Promise<td::BufferSlice> promise_;
  std::vector<tl_object_ptr<ton_api::tonNode_DataFull>> result_;

  static constexpr td::uint32 MAX_BLOCKS = 10;
  static constexpr size_t MAX_TOTAL_SIZE = 1 << 20;
};

class FullNodeQueryHandler {
 public:
  explicit FullNodeQueryHandler(td::actor::ActorId<ValidatorManagerInterface> manager) : manager_(manager) {
  }

  td::actor::Task<td::BufferSlice> handle_query(td::BufferSlice query, adnl::AdnlNodeIdShort src, QuerySource source) {
    auto f = CO_TRY(fetch_tl_object<ton_api::Function>(std::move(query), true));
    td::actor::StartedTask<td::BufferSlice> task;
    int tag = f->get_id();
    ton_api::downcast_call(*f.get(), [&](auto &obj) { task = process_query(std::move(obj), src, source).start(); });
    auto R = co_await std::move(task).wrap();
    auto name = [&]() -> const char * {
      auto s = ton_api_id_name(tag);
      return s ? s : "unknown";
    };
    if (R.is_ok()) {
      VLOG(full_node, DEBUG) << "Processed query (" << name() << ") in " << source << " from " << src << " : OK, "
                             << R.ok().size() << " bytes";
    } else {
      VLOG(full_node, DEBUG) << "Processed query (" << name() << ") in " << source << " from " << src << " : "
                             << R.error();
    }
    co_return R;
  }

 private:
  td::actor::ActorId<ValidatorManagerInterface> manager_;

  template <class T>
  td::actor::Task<td::BufferSlice> process_query(T, adnl::AdnlNodeIdShort, QuerySource) {
    co_return td::Status::Error(ErrorCode::error, "unknown query");
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_getNextBlockDescription query,
                                                 adnl::AdnlNodeIdShort src, QuerySource source) {
    BlockIdExt block_id = create_block_id(query.prev_block_);
    VLOG(full_node, DEBUG) << "Got query getNextBlockDescription " << block_id << " in " << source << " from " << src;
    if (!block_id.is_masterchain_ext()) {
      co_return td::Status::Error(ErrorCode::protoviolation, "next block allowed only for masterchain");
    }
    auto R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_next_block, block_id).wrap();
    if (R.is_error()) {
      co_return create_serialize_tl_object<ton_api::tonNode_blockDescriptionEmpty>();
    }
    auto handle = R.move_as_ok();
    if (!handle->received() || !handle->inited_proof()) {
      co_return create_serialize_tl_object<ton_api::tonNode_blockDescriptionEmpty>();
    }
    co_return create_serialize_tl_object<ton_api::tonNode_blockDescription>(create_tl_block_id(handle->id()));
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_prepareBlock query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    BlockIdExt block_id = create_block_id(query.block_);
    VLOG(full_node, DEBUG) << "Got query prepareBlock " << block_id << " in " << source << " from " << src;
    auto R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_block_handle, block_id, false).wrap();
    if (R.is_error()) {
      co_return create_serialize_tl_object<ton_api::tonNode_notFound>();
    }
    auto handle = R.move_as_ok();
    if (!handle->received()) {
      co_return create_serialize_tl_object<ton_api::tonNode_notFound>();
    }
    co_return create_serialize_tl_object<ton_api::tonNode_prepared>();
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_downloadBlock query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    BlockIdExt block_id = create_block_id(query.block_);
    VLOG(full_node, DEBUG) << "Got query downloadBlock " << block_id << " in " << source << " from " << src;
    auto handle = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_block_handle, block_id, false);
    if (!handle->received()) {
      co_return td::Status::Error(ErrorCode::notready, "unknown block");
    }
    co_return co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_block_data, handle);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_downloadBlockFull query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    BlockIdExt block_id = create_block_id(query.block_);
    VLOG(full_node, DEBUG) << "Got query downloadBlockFull " << block_id << " in " << source << " from " << src;
    auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::create_actor<BlockFullSender>(PSTRING() << "sender" << block_id.id, block_id, false, manager_,
                                             std::move(promise))
        .release();
    co_return co_await std::move(task);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_downloadNextBlockFull query,
                                                 adnl::AdnlNodeIdShort src, QuerySource source) {
    BlockIdExt prev_id = create_block_id(query.prev_block_);
    VLOG(full_node, DEBUG) << "Got query downloadNextBlockFull " << prev_id << " in " << source << " from " << src;
    auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::create_actor<BlockFullSender>(PSTRING() << "sender.next" << prev_id.id, prev_id, true, manager_,
                                             std::move(promise))
        .release();
    co_return co_await std::move(task);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_downloadNextBlocksFull query,
                                                 adnl::AdnlNodeIdShort src, QuerySource source) {
    BlockIdExt prev_id = create_block_id(query.prev_block_);
    td::uint32 max_blocks = query.max_blocks_;
    VLOG(full_node, DEBUG) << "Got query downloadNextBlocksFull " << prev_id << " max=" << max_blocks << " in "
                           << source << " from " << src;
    auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    td::actor::create_actor<NextBlocksFullSender>(PSTRING() << "sender.nexts" << prev_id.id, prev_id, query.max_blocks_,
                                                  manager_, std::move(promise))
        .release();
    co_return co_await std::move(task);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_prepareBlockProof query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    BlockIdExt block_id = create_block_id(query.block_);
    bool allow_partial = query.allow_partial_;
    VLOG(full_node, DEBUG) << "Got query prepareBlockProof " << block_id << " allow_partial=" << allow_partial << " in "
                           << source << " from " << src;
    if (block_id.seqno() == 0) {
      co_return td::Status::Error(ErrorCode::protoviolation, "cannot download proof for zero state");
    }
    auto R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_block_handle, block_id, false).wrap();
    if (R.is_error()) {
      co_return create_serialize_tl_object<ton_api::tonNode_preparedProofEmpty>();
    }
    auto handle = R.move_as_ok();
    if (!handle->inited_proof() && (!allow_partial || !handle->inited_proof_link())) {
      co_return create_serialize_tl_object<ton_api::tonNode_preparedProofEmpty>();
    }
    if (handle->inited_proof() && handle->id().is_masterchain()) {
      co_return create_serialize_tl_object<ton_api::tonNode_preparedProof>();
    }
    co_return create_serialize_tl_object<ton_api::tonNode_preparedProofLink>();
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_prepareKeyBlockProof query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    BlockIdExt block_id = create_block_id(query.block_);
    bool allow_partial = query.allow_partial_;
    VLOG(full_node, DEBUG) << "Got query prepareKeyBlockProof " << block_id << " allow_partial=" << allow_partial
                           << " in " << source << " from " << src;
    if (block_id.seqno() == 0) {
      co_return td::Status::Error(ErrorCode::protoviolation, "cannot download proof for zero state");
    }
    td::Result<td::BufferSlice> R;
    if (allow_partial) {
      R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_key_block_proof_link, block_id).wrap();
    } else {
      R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_key_block_proof, block_id).wrap();
    }
    if (R.is_error()) {
      co_return create_serialize_tl_object<ton_api::tonNode_preparedProofEmpty>();
    }
    if (allow_partial) {
      co_return create_serialize_tl_object<ton_api::tonNode_preparedProofLink>();
    }
    co_return create_serialize_tl_object<ton_api::tonNode_preparedProof>();
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_downloadBlockProof query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    BlockIdExt block_id = create_block_id(query.block_);
    VLOG(full_node, DEBUG) << "Got query downloadBlockProof " << block_id << " in " << source << " from " << src;
    auto handle = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_block_handle, block_id, false);
    if (!handle->inited_proof()) {
      co_return td::Status::Error(ErrorCode::notready, "unknown block proof");
    }
    co_return co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_block_proof, handle);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_downloadBlockProofLink query,
                                                 adnl::AdnlNodeIdShort src, QuerySource source) {
    BlockIdExt block_id = create_block_id(query.block_);
    VLOG(full_node, DEBUG) << "Got query downloadBlockProofLink " << block_id << " in " << source << " from " << src;
    auto handle = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_block_handle, block_id, false);
    if (!handle->inited_proof_link()) {
      co_return td::Status::Error(ErrorCode::notready, "unknown block proof");
    }
    co_return co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_block_proof_link, handle);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_downloadKeyBlockProof query,
                                                 adnl::AdnlNodeIdShort src, QuerySource source) {
    BlockIdExt block_id = create_block_id(query.block_);
    VLOG(full_node, DEBUG) << "Got query downloadKeyBlockProof " << block_id << " in " << source << " from " << src;
    if (query.block_->seqno_ == 0) {
      co_return td::Status::Error(ErrorCode::protoviolation, "cannot download proof for zero state");
    }
    co_return co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_key_block_proof, block_id);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_downloadKeyBlockProofLink query,
                                                 adnl::AdnlNodeIdShort src, QuerySource source) {
    BlockIdExt block_id = create_block_id(query.block_);
    VLOG(full_node, DEBUG) << "Got query downloadKeyBlockProofLink " << block_id << " in " << source << " from " << src;
    if (query.block_->seqno_ == 0) {
      co_return td::Status::Error(ErrorCode::protoviolation, "cannot download proof for zero state");
    }
    co_return co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_key_block_proof_link, block_id);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_prepareZeroState query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    BlockIdExt block_id = create_block_id(query.block_);
    VLOG(full_node, DEBUG) << "Got query prepareZeroState " << block_id << " in " << source << " from " << src;
    auto R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::check_zero_state_exists, block_id).wrap();
    if (R.is_error() || !R.move_as_ok()) {
      co_return create_serialize_tl_object<ton_api::tonNode_notFoundState>();
    }
    co_return create_serialize_tl_object<ton_api::tonNode_preparedState>();
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_preparePersistentState query,
                                                 adnl::AdnlNodeIdShort src, QuerySource source) {
    auto block_id = create_block_id(query.block_);
    auto masterchain_block_id = create_block_id(query.masterchain_block_);
    VLOG(full_node, DEBUG) << "Got query preparePersistentState " << block_id << " " << masterchain_block_id << " in "
                           << source << " from " << src;
    auto R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_persistent_state_size, block_id,
                                     masterchain_block_id, UnsplitStateType{})
                 .wrap();
    if (R.is_error()) {
      co_return create_serialize_tl_object<ton_api::tonNode_notFoundState>();
    }
    co_return create_serialize_tl_object<ton_api::tonNode_preparedState>();
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_getNextKeyBlockIds query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    auto cnt = static_cast<td::uint32>(query.max_size_);
    if (cnt > 8) {
      cnt = 8;
    }
    auto block_id = create_block_id(query.block_);
    VLOG(full_node, DEBUG) << "Got query getNextKeyBlockIds " << block_id << " " << cnt << " in " << source << " from "
                           << src;
    auto R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_next_key_blocks, block_id, cnt).wrap();
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::notready) {
        VLOG(full_node, DEBUG) << "getnextkey: " << R.move_as_error();
      } else {
        VLOG(full_node, WARNING) << "getnextkey: " << R.move_as_error();
      }
      co_return create_serialize_tl_object<ton_api::tonNode_keyBlocks>(
          std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>>{}, false, true);
    }
    auto res = R.move_as_ok();
    std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>> v;
    for (auto &b : res) {
      v.emplace_back(create_tl_block_id(b));
    }
    co_return create_serialize_tl_object<ton_api::tonNode_keyBlocks>(std::move(v), res.size() < cnt, false);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_downloadZeroState query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    auto block_id = create_block_id(query.block_);
    VLOG(full_node, DEBUG) << "Got query downloadZeroState " << block_id << " in " << source << " from " << src;
    co_return co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_zero_state, block_id);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_getCapabilities, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    VLOG(full_node, DEBUG) << "Got query getCapabilities in " << source << " from " << src;
    co_return create_serialize_tl_object<ton_api::tonNode_capabilities>(FullNode::PROTO_VERSION_MAJOR,
                                                                        FullNode::PROTO_VERSION_MINOR, 0);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_getArchiveInfo query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    BlockSeqno masterchain_seqno = query.masterchain_seqno_;
    VLOG(full_node, DEBUG) << "Got query getArchiveInfo " << masterchain_seqno << " in " << source << " from " << src;
    auto R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_archive_id, query.masterchain_seqno_,
                                     ShardIdFull{masterchainId})
                 .wrap();
    if (R.is_error()) {
      co_return create_serialize_tl_object<ton_api::tonNode_archiveNotFound>();
    }
    co_return create_serialize_tl_object<ton_api::tonNode_archiveInfo>(R.move_as_ok());
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_getShardArchiveInfo query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    BlockSeqno masterchain_seqno = query.masterchain_seqno_;
    ShardIdFull shard_prefix = create_shard_id(query.shard_prefix_);
    VLOG(full_node, DEBUG) << "Got query getShardArchiveInfo " << masterchain_seqno << " " << shard_prefix << " in "
                           << source << " from " << src;
    auto R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_archive_id, query.masterchain_seqno_,
                                     shard_prefix)
                 .wrap();
    if (R.is_error()) {
      co_return create_serialize_tl_object<ton_api::tonNode_archiveNotFound>();
    }
    co_return create_serialize_tl_object<ton_api::tonNode_archiveInfo>(R.move_as_ok());
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_getArchiveSlice query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    VLOG(full_node, DEBUG) << "Got query getArchiveInfo " << query.archive_id_ << " " << query.offset_ << " "
                           << query.max_size_ << " in " << source << " from " << src;
    if (query.max_size_ < 0 || query.max_size_ > (1 << 24)) {
      co_return td::Status::Error(ErrorCode::protoviolation, "invalid max_size");
    }
    co_return co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_archive_slice, query.archive_id_,
                                      query.offset_, query.max_size_);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_downloadPersistentStateSliceV2 query,
                                                 adnl::AdnlNodeIdShort src, QuerySource source) {
    auto [block_id, mc_block_id, state_type] = persistent_state_from_v2_query(query);
    VLOG(full_node, DEBUG) << "Got query downloadPersistentStateSliceV2 " << block_id << " " << mc_block_id << " ("
                           << persistent_state_type_to_string(block_id.shard_full(), state_type) << ") "
                           << query.offset_ << " " << query.max_size_ << " in " << source << " from " << src;
    if (query.max_size_ < 0 || query.max_size_ > (1 << 24)) {
      co_return td::Status::Error(ErrorCode::protoviolation, "invalid max_size");
    }
    co_return co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_persistent_state_slice, block_id,
                                      mc_block_id, state_type, query.offset_, query.max_size_);
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_getPersistentStateSizeV2 query,
                                                 adnl::AdnlNodeIdShort src, QuerySource source) {
    auto [block_id, mc_block_id, state_type] = persistent_state_from_v2_query(query);
    VLOG(full_node, DEBUG) << "Got query getPersistentStateSizeV2 " << block_id << " " << mc_block_id << " ("
                           << persistent_state_type_to_string(block_id.shard_full(), state_type) << ") in " << source
                           << " from " << src;
    auto R = co_await td::actor::ask(manager_, &ValidatorManagerInterface::get_persistent_state_size, block_id,
                                     mc_block_id, state_type)
                 .wrap();
    if (R.is_error()) {
      co_return create_serialize_tl_object<ton_api::tonNode_persistentStateSizeNotFound>();
    }
    co_return create_serialize_tl_object<ton_api::tonNode_persistentStateSize>(R.move_as_ok());
  }

  td::actor::Task<td::BufferSlice> process_query(ton_api::tonNode_slave_sendExtMessage query, adnl::AdnlNodeIdShort src,
                                                 QuerySource source) {
    VLOG(full_node, DEBUG) << "Got query slave.sendExtMessage in " << source << " from " << src;
    if (source != QuerySource::full_node_master) {
      co_return td::Status::Error(ErrorCode::protoviolation, "query not from full-node master");
    }
    td::actor::send_closure(
        manager_, &ValidatorManagerInterface::run_ext_query,
        create_serialize_tl_object<lite_api::liteServer_query>(
            create_serialize_tl_object<lite_api::liteServer_sendMessage>(std::move(query.message_->data_))),
        [&](td::Result<td::BufferSlice>) {});
    co_return create_serialize_tl_object<ton_api::tonNode_success>();
  }
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
