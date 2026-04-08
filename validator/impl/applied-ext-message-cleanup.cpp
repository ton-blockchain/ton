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
*/

#include <algorithm>

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "ton/ton-io.hpp"
#include "vm/dict.h"

#include "applied-ext-message-cleanup.hpp"
#include "external-message.hpp"

namespace ton::validator {

using td::Ref;

namespace {

td::Result<std::vector<ExtMessage::Hash>> get_applied_external_messages_hashes(td::Ref<BlockData> block) {
  if (block.is_null()) {
    return std::vector<ExtMessage::Hash>{};
  }
  try {
    block::gen::Block::Record blk;
    block::gen::BlockExtra::Record extra;
    auto block_root = block->root_cell();
    if (!(tlb::unpack_cell(block_root, blk) && tlb::unpack_cell(blk.extra, extra))) {
      return td::Status::Error("cannot unpack applied block");
    }

    vm::AugmentedDictionary in_msg_dict{vm::load_cell_slice_ref(extra.in_msg_descr), 256,
                                        block::tlb::aug_InMsgDescrDefault};
    std::vector<ExtMessage::Hash> hashes;
    td::Status error;
    if (!in_msg_dict.check_for_each_extra(
            [&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr, int key_len) {
              if (key_len != 256) {
                error = td::Status::Error("invalid InMsgDescr key length");
                return false;
              }
              int tag = block::gen::t_InMsg.get_tag(*value);
              if (tag != block::gen::InMsg::msg_import_ext) {
                return true;
              }
              vm::CellSlice cs{*value};
              Ref<vm::Cell> msg, transaction;
              if (!block::gen::t_InMsg.unpack_msg_import_ext(cs, msg, transaction)) {
                error = td::Status::Error("cannot unpack msg_import_ext");
                return false;
              }
              auto hash = get_ext_in_msg_hash_norm(msg);
              if (hash.is_error()) {
                error = hash.move_as_error_prefix("cannot normalize applied external message: ");
                return false;
              }
              hashes.push_back(hash.move_as_ok());
              return true;
            })) {
      if (error.is_error()) {
        return std::move(error);
      }
      return td::Status::Error("failed to iterate applied block InMsgDescr");
    }

    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    return hashes;
  } catch (vm::VmError &err) {
    return td::Status::Error(PSTRING() << "error while parsing applied block " << block->block_id().to_str() << ": "
                                       << err.get_msg());
  } catch (vm::VmVirtError &err) {
    return td::Status::Error(PSTRING() << "virtualization error while parsing applied block "
                                       << block->block_id().to_str() << ": " << err.get_msg());
  }
}

}  // namespace

void AppliedExtMessageCleanupActor::got_block_data(BlockIdExt block_id, td::Result<td::Ref<BlockData>> block) {
  if (block.is_error()) {
    LOG(WARNING) << "failed to load block data for applied external cleanup of block " << block_id.to_str() << " : "
                 << block.move_as_error();
    return;
  }
  cleanup_applied_block(BlockHandle{}, block.move_as_ok());
}

void AppliedExtMessageCleanupActor::cleanup_applied_block(BlockHandle handle, td::Ref<BlockData> block) {
  if (block.is_null()) {
    if (!handle) {
      return;
    }
    auto block_id = handle->id();
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block_id](td::Result<td::Ref<BlockData>> R) mutable {
      td::actor::send_closure(SelfId, &AppliedExtMessageCleanupActor::got_block_data, block_id, std::move(R));
    });
    td::actor::send_closure(manager_, &ValidatorManager::get_block_data_from_db, handle, std::move(P));
    return;
  }

  auto hashes = get_applied_external_messages_hashes(block);
  if (hashes.is_error()) {
    LOG(WARNING) << "failed to cleanup applied externals for block "
                 << (block.is_null() ? "(null)" : block->block_id().to_str()) << " : " << hashes.move_as_error();
    return;
  }
  auto values = hashes.move_as_ok();
  if (values.empty()) {
    return;
  }
  LOG(INFO) << "cleanup applied externals for block " << block->block_id().to_str()
            << " : normalized_hashes=" << values.size();
  td::actor::send_closure(ext_message_pool_, &ExtMessagePool::erase_external_messages, std::move(values));
}

}  // namespace ton::validator
