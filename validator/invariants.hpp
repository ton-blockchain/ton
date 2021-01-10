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

#include "validator/interfaces/block-handle.h"

namespace ton {

namespace validator {

class ValidatorInvariants {
 public:
  static void check_post_apply(BlockHandle handle) {
    CHECK(handle->received_state());
    CHECK(handle->inited_state_root_hash());
    CHECK(handle->inited_logical_time());
    CHECK(handle->inited_unix_time());
    CHECK(handle->inited_split_after());
    if (handle->id().seqno() > 0) {
      CHECK(handle->inited_proof() || handle->inited_proof_link());
    }
    CHECK(handle->processed());
    CHECK(handle->is_applied());
  }
  static void check_post_accept(BlockHandle handle) {
    CHECK(handle->received());
    CHECK(handle->received_state());
    CHECK(handle->inited_state_root_hash());
    CHECK(handle->inited_merge_before());
    CHECK(handle->inited_split_after());
    CHECK(handle->inited_prev());
    CHECK(handle->inited_signatures() || handle->is_applied());
    CHECK(handle->inited_state_root_hash());
    CHECK(handle->inited_logical_time());
    CHECK(handle->inited_unix_time());
    if (handle->id().is_masterchain()) {
      CHECK(handle->inited_proof());
      CHECK(handle->is_applied());
      CHECK(handle->inited_is_key_block());
    } else {
      CHECK(handle->inited_proof_link());
    }
  }
  static void check_post_check_proof(BlockHandle handle) {
    CHECK(handle->inited_merge_before());
    CHECK(handle->inited_split_after());
    CHECK(handle->inited_prev());
    CHECK(handle->inited_state_root_hash());
    CHECK(handle->inited_logical_time());
    CHECK(handle->inited_unix_time());
    CHECK(handle->inited_proof());
    CHECK(handle->inited_is_key_block());
  }
  static void check_post_check_proof_link(BlockHandle handle) {
    CHECK(handle->inited_merge_before());
    CHECK(handle->inited_split_after());
    CHECK(handle->inited_prev());
    CHECK(handle->inited_state_root_hash());
    CHECK(handle->inited_logical_time());
    CHECK(handle->inited_unix_time());
    CHECK(handle->inited_proof_link());
  }
};

}  // namespace validator

}  // namespace ton
