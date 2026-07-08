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
#include "adnl/utils.hpp"
#include "td/utils/overloaded.h"
#include "ton/ton-io.hpp"
#include "ton/ton-shard.h"
#include "ton/ton-tl.hpp"
#include "validator/full-node.h"

#include "download-next-blocks.hpp"
#include "full-node-serializer.hpp"

namespace ton {

namespace validator {

namespace fullnode {

DownloadNextBlocks::DownloadNextBlocks(BlockHandle handle, QuerySender query_sender, td::uint32 priority,
                                       td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                       td::Promise<BlockHandle> promise)
    : handle_(handle)
    , start_prev_id_(handle->id())
    , query_sender_(std::move(query_sender))
    , priority_(priority)
    , validator_manager_(validator_manager)
    , promise_(std::move(promise)) {
}

void DownloadNextBlocks::start_up() {
  [](DownloadNextBlocks *self) -> td::actor::Task<> {
    auto R = co_await self->run().start().wrap();
    td::StringBuilder sb;
    if (self->success_local_) {
      sb << "loaded next block after " << self->start_prev_id_.id << " from db";
    } else if (self->success_) {
      sb << "downloaded next blocks after " << self->start_prev_id_.id << " from " << self->query_sender_->to_str()
         << " up to " << self->handle_->id().id;
    }
    if (R.is_error()) {
      if (self->success_) {
        sb << ", then got error: " << R.error();
      } else {
        sb << "failed to download next blocks after " << self->start_prev_id_.id << " from "
           << self->query_sender_->to_str() << ": " << R.error();
      }
    }
    if (R.is_ok() || R.error().code() == ErrorCode::notready || R.error().code() == ErrorCode::timeout) {
      VLOG(full_node, DEBUG) << sb.as_cslice();
    } else {
      VLOG(full_node, WARNING) << sb.as_cslice();
    }
    if (self->success_) {
      self->promise_.set_value(std::move(self->handle_));
    } else {
      R.ensure_error();
      self->promise_.set_error(R.move_as_error());
    }
    self->stop();
    co_return {};
  }(this)
                                      .start()
                                      .detach();
}

td::actor::Task<> DownloadNextBlocks::run() {
  CHECK(start_prev_id_.is_masterchain_ext());
  VLOG(full_node, DEBUG) << "Download next block after " << start_prev_id_;
  if (handle_->inited_next()) {
    auto next_handle = co_await td::actor::ask(validator_manager_, &ValidatorManagerInterface::get_block_handle,
                                               handle_->one_next(true), true);
    if (next_handle->inited_proof() && next_handle->received()) {
      VLOG(full_node, DEBUG) << "Next block already stored";
      auto block =
          co_await td::actor::ask(validator_manager_, &ValidatorManagerInterface::get_block_data_from_db, next_handle);
      ReceivedBlock result{.id = block->block_id(), .data = block->data()};
      handle_ = co_await td::actor::ask(validator_manager_, &ValidatorManagerInterface::got_next_masterchain_block,
                                        std::move(result));
      success_ = success_local_ = true;
      co_return {};
    }
  }

  bool allow_many = query_sender_->get_proto_version() >= std::make_pair<td::uint32, td::uint32>(3, 2);
  VLOG(full_node, DEBUG) << "Download from " << query_sender_->to_str() << ", allow_many=" << allow_many;
  td::BufferSlice query;
  size_t max_size;
  if (allow_many) {
    query = create_serialize_tl_object<ton_api::tonNode_downloadNextBlocksFull>(create_tl_block_id(start_prev_id_),
                                                                                MAX_BLOCKS);
    max_size = std::max<size_t>(MAX_SIZE_MANY, FullNode::max_proof_size() + FullNode::max_block_size() + 128);
  } else {
    query = create_serialize_tl_object<ton_api::tonNode_downloadNextBlockFull>(create_tl_block_id(start_prev_id_));
    max_size = FullNode::max_proof_size() + FullNode::max_block_size() + 128;
  }

  td::BufferSlice response = co_await query_sender_->send_query(std::move(query), td::Timestamp::in(5.0), max_size);
  std::vector<tl_object_ptr<ton_api::tonNode_DataFull>> response_vec;
  if (allow_many) {
    auto f = CO_TRY(fetch_tl_object<ton_api::tonNode_nextBlocksFull>(std::move(response), true));
    for (auto &obj : f->blocks_) {
      if (obj->get_id() == ton_api::tonNode_dataFullEmpty::ID) {
        co_return td::Status::Error(ErrorCode::protoviolation, "got empty block in nextBlocksFull");
      }
    }
    response_vec = std::move(f->blocks_);
  } else {
    auto f = CO_TRY(fetch_tl_object<ton_api::tonNode_DataFull>(std::move(response), true));
    if (f->get_id() != ton_api::tonNode_dataFullEmpty::ID) {
      response_vec.push_back(std::move(f));
    }
  }
  if (response_vec.empty()) {
    co_return td::Status::Error(ErrorCode::notready, "node doesn't have next blocks");
  }
  VLOG(full_node, DEBUG) << "Got response, " << response_vec.size() << " blocks";

  for (auto &obj : response_vec) {
    co_await process_block(std::move(obj));
  }
  VLOG(full_node, DEBUG) << "Done";
  co_return {};
}

td::actor::Task<> DownloadNextBlocks::process_block(tl_object_ptr<ton_api::tonNode_DataFull> obj) {
  auto requires_state = CO_TRY(need_state_for_decompression(*obj).trace("need state for decompression"));
  Ref<vm::Cell> prev_state_root;
  if (requires_state) {
    CHECK(obj->get_id() == ton_api::tonNode_dataFullCompressedV2::ID);
    auto compressed_v2 = static_cast<const ton_api::tonNode_dataFullCompressedV2 *>(obj.get());
    BlockIdExt id = create_block_id(compressed_v2->id_);
    auto prev_blocks =
        CO_TRY(extract_prev_blocks_from_proof(compressed_v2->proof_.as_slice(), id).trace("extract prev blocks"));
    if (prev_blocks != std::vector{handle_->id()}) {
      co_return td::Status::Error(ErrorCode::protoviolation, "prev block id mismatch");
    }
    auto prev_state = co_await td::actor::ask(validator_manager_, &ValidatorManagerInterface::wait_block_state, handle_,
                                              priority_, td::Timestamp::in(5.0), true)
                          .trace("wait prev state");
    prev_state_root = prev_state->root_cell();
  }

  BlockIdExt id;
  td::BufferSlice proof, block_data;
  bool is_link;
  CO_TRY(deserialize_block_full(*obj, id, proof, block_data, is_link, overlay::Overlays::max_fec_broadcast_size(),
                                prev_state_root)
             .trace("deserialize block"));
  if (is_link) {
    co_return td::Status::Error(ErrorCode::protoviolation, "node doesn't have proof for this block");
  }
  if (td::sha256_bits256(block_data.as_slice()) != id.file_hash) {
    co_return td::Status::Error(ErrorCode::protoviolation, "received data with bad hash");
  }
  co_await td::actor::ask(validator_manager_, &ValidatorManagerInterface::validate_block_is_next_proof, handle_->id(),
                          id, std::move(proof));
  ReceivedBlock result{.id = id, .data = std::move(block_data)};
  handle_ = co_await td::actor::ask(validator_manager_, &ValidatorManagerInterface::got_next_masterchain_block,
                                    std::move(result));
  success_ = true;
  VLOG(full_node, DEBUG) << "Downloaded block " << id;
  co_return {};
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
