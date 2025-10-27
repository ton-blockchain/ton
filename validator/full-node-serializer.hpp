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
#pragma once
#include "ton/ton-types.h"
#include "auto/tl/ton_api.h"
#include "vm/cells.h"
#include "td/actor/actor.h"
#include "td/utils/overloaded.h"
#include "validator/validator.h"
#include "interfaces/shard.h"
#include "ton/ton-tl.hpp"

namespace ton::validator::fullnode {

enum class StateUsage {
  None,
  DecompressOnly,
  CompressAndDecompress
};

td::Result<td::BufferSlice> serialize_block_broadcast(const BlockBroadcast& broadcast, bool compression_enabled,
                                                      StateUsage state_usage = StateUsage::None,
                                                      td::Ref<vm::Cell> state = td::Ref<vm::Cell>());
td::Result<BlockBroadcast> deserialize_block_broadcast(ton_api::tonNode_Broadcast& obj, int max_decompressed_data_size, 
                                                       td::Ref<vm::Cell> state = td::Ref<vm::Cell>());

td::Result<std::vector<BlockIdExt>> extract_prev_blocks_from_proof(td::Slice proof, const BlockIdExt& block_id);

td::Result<bool> need_state_for_decompression(ton_api::tonNode_Broadcast& broadcast);
td::Result<bool> need_state_for_decompression(ton_api::tonNode_DataFull& data_full);

td::Result<td::BufferSlice> serialize_block_full(const BlockIdExt& id, td::Slice proof, td::Slice data,
                                                 bool is_proof_link, bool compression_enabled,
                                                 StateUsage state_usage = StateUsage::None,
                                                 td::Ref<vm::Cell> state = td::Ref<vm::Cell>());
td::Status deserialize_block_full(ton_api::tonNode_DataFull& obj, BlockIdExt& id, td::BufferSlice& proof,
                                  td::BufferSlice& data, bool& is_proof_link, int max_decompressed_data_size,
                                  td::Ref<vm::Cell> state = td::Ref<vm::Cell>());

td::Result<td::BufferSlice> serialize_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                                td::uint32 validator_set_hash, td::Slice data,
                                                                bool compression_enabled);
td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_Broadcast& obj, BlockIdExt& block_id,
                                                 CatchainSeqno& cc_seqno, td::uint32& validator_set_hash,
                                                 td::BufferSlice& data, int max_decompressed_data_size);

// Template method that asyncroniously obtains state and decompresses broadcast with it
// Should be called only for broadcasts that require state for decompression
template<typename ActorT>
void process_broadcast_with_async_state(
    ton_api::tonNode_Broadcast& query,
    PublicKeyHash src,
    td::actor::ActorId<ValidatorManagerInterface> validator_manager,
    td::actor::ActorId<ActorT> self_actor,
    void (ActorT::*callback)(PublicKeyHash, ton_api::tonNode_blockBroadcastCompressedV2, td::Result<td::Ref<ShardState>>)
) {
  ton_api::downcast_call(
    query,
    td::overloaded(
      [&](ton_api::tonNode_blockBroadcastCompressedV2 &f) {
        auto R_prev_blocks = extract_prev_blocks_from_proof(f.proof_, create_block_id(f.id_));
        if (R_prev_blocks.is_error()) {
          LOG(DEBUG) << "Failed to extract prev block IDs from V2 broadcast: " << R_prev_blocks.move_as_error();
          return;
        }
        auto prev_blocks = R_prev_blocks.move_as_ok();
        
        // Request previous block state(s) asynchronously
        if (prev_blocks.size() == 1) {
          LOG(DEBUG) << "Requesting state for single prev block " << prev_blocks[0].to_str();
          td::actor::send_closure(validator_manager, &ValidatorManagerInterface::wait_block_state_short,
                                 prev_blocks[0], 0, td::Timestamp::in(10.0), false,
                                 [self_actor, src, callback, query_moved = std::move(f)](
                                   td::Result<td::Ref<ShardState>> R) mutable {
                                   td::actor::send_closure(self_actor, callback, src, std::move(query_moved), std::move(R));
                                 });
        } else {
          CHECK(prev_blocks.size() == 2);
          LOG(DEBUG) << "Requesting merged state for prev blocks " << prev_blocks[0].to_str() 
                     << " and " << prev_blocks[1].to_str();
          td::actor::send_closure(validator_manager, &ValidatorManagerInterface::wait_block_state_merge,
                                 prev_blocks[0], prev_blocks[1], 0, td::Timestamp::in(10.0),
                                 [self_actor, src, callback, query_moved = std::move(f)](
                                   td::Result<td::Ref<ShardState>> R) mutable {
                                   td::actor::send_closure(self_actor, callback, src, std::move(query_moved), std::move(R));
                                 });
        }
      },
      [&](auto &) {
        // Other broadcast types don't need state
        UNREACHABLE();
      }
    )
  );
}

}  // namespace ton::validator::fullnode
