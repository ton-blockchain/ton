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
#include "get-next-key-blocks.h"

namespace ton {

namespace validator {

void GetNextKeyBlocks::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(res_));
  }
  stop();
}

void GetNextKeyBlocks::abort_query(td::Status error) {
  if (promise_) {
    promise_.set_error(std::move(error));
  }
  stop();
}

void GetNextKeyBlocks::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void GetNextKeyBlocks::start_up() {
  if (!block_id_.is_masterchain()) {
    abort_query(td::Status::Error(ErrorCode::notready, "block is not from masterchain"));
    return;
  }

  if (block_id_.id.seqno > last_known_key_block_->id().seqno()) {
    finish_query();
    return;
  }

  if (block_id_.id.seqno == last_known_key_block_->id().seqno()) {
    if (block_id_ == last_known_key_block_->id()) {
      finish_query();
    } else {
      abort_query(td::Status::Error(ErrorCode::protoviolation, "bad block id"));
    }
    return;
  }

  if (block_id_.seqno() > 0) {
    BlockIdExt block_id;
    auto f = masterchain_state_->prev_key_block_id(block_id_.seqno());
    if (!f.is_valid()) {
      abort_query(td::Status::Error(ErrorCode::protoviolation, "bad block id (not key?)"));
      return;
    } else if (f != block_id_) {
      abort_query(td::Status::Error(ErrorCode::protoviolation, "bad block id (not key?) 2"));
      return;
    }
  }

  while (res_.size() < limit_) {
    auto b = masterchain_state_->next_key_block_id(block_id_.seqno() + 1);
    if (b.is_valid()) {
      res_.push_back(b);
      block_id_ = b;
    } else {
      finish_query();
      return;
    }
  }

  finish_query();
}

}  // namespace validator

}  // namespace ton
