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

namespace ton::validator::fullnode {

td::Result<td::BufferSlice> serialize_block_broadcast(const BlockBroadcast& broadcast, bool compression_enabled);
td::Result<BlockBroadcast> deserialize_block_broadcast(ton_api::tonNode_Broadcast& obj, int max_decompressed_data_size);

td::Result<td::BufferSlice> serialize_block_full(const BlockIdExt& id, td::Slice proof, td::Slice data,
                                                 bool is_proof_link, bool compression_enabled);
td::Status deserialize_block_full(ton_api::tonNode_DataFull& obj, BlockIdExt& id, td::BufferSlice& proof,
                                  td::BufferSlice& data, bool& is_proof_link, int max_decompressed_data_size);

td::Result<td::BufferSlice> serialize_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                                td::uint32 validator_set_hash, td::Slice data,
                                                                td::optional<td::Slice> collated_data);
td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_Broadcast& obj, BlockIdExt& block_id,
                                                 CatchainSeqno& cc_seqno, td::uint32& validator_set_hash,
                                                 td::BufferSlice& data, td::optional<td::BufferSlice>& collated_data,
                                                 int max_decompressed_data_size);

td::Result<td::BufferSlice> serialize_block_candidate_data(BlockCandidate candidate, bool only_collated_data,
                                                           bool compression_enabled);
td::Status deserialize_block_candidate_data(ton_api::tonNode_BlockCandidateData& obj, bool only_collated_data,
                                            BlockIdExt& id, td::BufferSlice& data, td::BufferSlice& collated_data,
                                            int max_decompressed_data_size);

}  // namespace ton::validator::fullnode
