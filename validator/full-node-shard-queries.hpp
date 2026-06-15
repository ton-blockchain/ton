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

#include "td/utils/tl_helpers.h"
#include "ton/ton-tl.hpp"
#include "validator/validator.h"

#include "full-node-serializer.hpp"

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
        LOG(DEBUG) << "NextBlocksFullSender error (sending " << self->result_.size()
                   << " blocks): " << R.move_as_error();
      } else {
        LOG(DEBUG) << "NextBlocksFullSender OK (sending " << self->result_.size() << " blocks)";
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

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
