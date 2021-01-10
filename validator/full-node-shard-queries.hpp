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

#include "validator/validator.h"
#include "ton/ton-tl.hpp"

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
    promise_.set_value(create_serialize_tl_object<ton_api::tonNode_dataFull>(
        create_tl_block_id(block_id_), std::move(proof_), std::move(data_), is_proof_link_));
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

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
