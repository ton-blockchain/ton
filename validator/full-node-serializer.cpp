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
#include "auto/tl/ton_api.hpp"
#include "td/utils/lz4.h"
#include "td/utils/overloaded.h"
#include "tl-utils/common-utils.hpp"
#include "ton/ton-io.hpp"
#include "ton/ton-tl.hpp"
#include "vm/boc-compression.h"
#include "vm/boc.h"

#include "full-node-serializer.hpp"
#include "full-node.h"

namespace ton::validator::fullnode {

td::Result<tl_object_ptr<ton_api::tonNode_DataFull>> serialize_block_full_obj(const BlockIdExt& id, td::Slice proof,
                                                                              td::Slice data, bool is_proof_link) {
  TRY_RESULT(data_root, vm::std_boc_deserialize(data));
  TRY_RESULT(compressed, vm::boc_compress({data_root}, vm::CompressionAlgorithm::ImprovedStructureLZ4));
  size_t compressed_size = compressed.size() + proof.size();
  VLOG(full_node, DEBUG) << "Compressing block full: " << data.size() + proof.size() << " -> " << compressed_size;
  auto res = create_tl_object<ton_api::tonNode_dataFullCompressedV2>(create_tl_block_id(id), 0, td::BufferSlice(proof),
                                                                     std::move(compressed), is_proof_link);
  return res;
}

td::Result<td::BufferSlice> serialize_block_full(const BlockIdExt& id, td::Slice proof, td::Slice data,
                                                 bool is_proof_link) {
  TRY_RESULT(obj, serialize_block_full_obj(id, proof, data, is_proof_link));
  return serialize_tl_object(obj, true);
}

static td::Status deserialize_block_full(ton_api::tonNode_dataFull& f, BlockIdExt& id, td::BufferSlice& proof,
                                         td::BufferSlice& data, bool& is_proof_link) {
  id = create_block_id(f.id_);
  proof = std::move(f.proof_);
  data = std::move(f.block_);
  is_proof_link = f.is_link_;
  return td::Status::OK();
}

static td::Status deserialize_block_full(ton_api::tonNode_dataFullCompressed& f, BlockIdExt& id, td::BufferSlice& proof,
                                         td::BufferSlice& data, bool& is_proof_link, int max_decompressed_size) {
  id = create_block_id(f.id_);

  TRY_RESULT(decompressed, td::lz4_decompress(f.compressed_, max_decompressed_size));
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed, 2));
  if (roots.size() != 2) {
    return td::Status::Error("expected 2 roots in boc");
  }
  TRY_RESULT_ASSIGN(proof, vm::std_boc_serialize(roots[0], 0));
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(roots[1], 31));
  VLOG(full_node, DEBUG) << "Decompressing block full: " << f.compressed_.size() << " -> "
                         << data.size() + proof.size();
  is_proof_link = f.is_link_;
  return td::Status::OK();
}

static td::Status deserialize_block_full(ton_api::tonNode_dataFullCompressedV2& f, BlockIdExt& id,
                                         td::BufferSlice& proof, td::BufferSlice& data, bool& is_proof_link,
                                         int max_decompressed_size) {
  id = create_block_id(f.id_);

  TRY_RESULT(roots, vm::boc_decompress(f.block_compressed_, max_decompressed_size));
  if (roots.size() != 1) {
    return td::Status::Error("expected 1 root in boc");
  }
  if (roots[0]->get_level() > 0) {
    return td::Status::Error("expected nonzero level root");
  }
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(roots[0], 31));
  proof = std::move(f.proof_);
  VLOG(full_node, DEBUG) << "Decompressing block full V2: " << f.block_compressed_.size() + f.proof_.size() << " -> "
                         << data.size() + proof.size();
  is_proof_link = f.is_link_;
  return td::Status::OK();
}

td::Status deserialize_block_full(ton_api::tonNode_DataFull& obj, BlockIdExt& id, td::BufferSlice& proof,
                                  td::BufferSlice& data, bool& is_proof_link, int max_decompressed_data_size) {
  td::Status S;
  ton_api::downcast_call(
      obj, td::overloaded(
               [&](ton_api::tonNode_dataFull& f) { S = deserialize_block_full(f, id, proof, data, is_proof_link); },
               [&](ton_api::tonNode_dataFullCompressed& f) {
                 S = deserialize_block_full(f, id, proof, data, is_proof_link, max_decompressed_data_size);
               },
               [&](ton_api::tonNode_dataFullCompressedV2& f) {
                 S = deserialize_block_full(f, id, proof, data, is_proof_link, max_decompressed_data_size);
               },
               [&](auto&) { S = td::Status::Error("unknown data type"); }));
  return S;
}

td::Result<td::BufferSlice> serialize_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                                td::uint32 validator_set_hash, td::Slice data) {
  TRY_RESULT(root, vm::std_boc_deserialize(data));
  TRY_RESULT(compressed, vm::boc_compress({root}, vm::CompressionAlgorithm::ImprovedStructureLZ4));
  auto compressed_size = compressed.size();
  VLOG(full_node, DEBUG) << "Compressing block candidate broadcast: " << data.size() << " -> " << compressed_size;
  auto res = create_serialize_tl_object<ton_api::tonNode_newBlockCandidateBroadcastCompressedV2>(
      create_tl_block_id(block_id), cc_seqno, validator_set_hash,
      create_tl_object<ton_api::tonNode_blockSignature>(Bits256::zero(), td::BufferSlice()), 0, std::move(compressed));
  return res;
}

static td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_newBlockCandidateBroadcastCompressed& obj,
                                                        BlockIdExt& block_id, CatchainSeqno& cc_seqno,
                                                        td::uint32& validator_set_hash, td::BufferSlice& data,
                                                        int max_decompressed_data_size) {
  block_id = create_block_id(obj.id_);
  cc_seqno = obj.catchain_seqno_;
  validator_set_hash = obj.validator_set_hash_;
  TRY_RESULT(decompressed, td::lz4_decompress(obj.compressed_, max_decompressed_data_size));
  TRY_RESULT(root, vm::std_boc_deserialize(decompressed));
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(root, 31));
  VLOG(full_node, DEBUG) << "Decompressing block candidate broadcast: " << obj.compressed_.size() << " -> "
                         << data.size();
  return td::Status::OK();
}

static td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_newBlockCandidateBroadcastCompressedV2& obj,
                                                        BlockIdExt& block_id, CatchainSeqno& cc_seqno,
                                                        td::uint32& validator_set_hash, td::BufferSlice& data,
                                                        int max_decompressed_data_size) {
  block_id = create_block_id(obj.id_);
  cc_seqno = obj.catchain_seqno_;
  validator_set_hash = obj.validator_set_hash_;
  TRY_RESULT(roots, vm::boc_decompress(obj.compressed_, max_decompressed_data_size));
  if (roots.size() != 1) {
    return td::Status::Error("expected 1 root in boc");
  }
  if (roots[0]->get_level() > 0) {
    return td::Status::Error("expected nonzero level root");
  }
  auto root = std::move(roots[0]);
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(root, 31));
  VLOG(full_node, DEBUG) << "Decompressing block candidate broadcast V2: " << obj.compressed_.size() << " -> "
                         << data.size();
  return td::Status::OK();
}

td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_Broadcast& obj, BlockIdExt& block_id,
                                                 CatchainSeqno& cc_seqno, td::uint32& validator_set_hash,
                                                 td::BufferSlice& data, int max_decompressed_data_size) {
  td::Status S;
  ton_api::downcast_call(obj, td::overloaded(
                                  [&](ton_api::tonNode_newBlockCandidateBroadcastCompressed& f) {
                                    S = deserialize_block_candidate_broadcast(f, block_id, cc_seqno, validator_set_hash,
                                                                              data, max_decompressed_data_size);
                                  },
                                  [&](ton_api::tonNode_newBlockCandidateBroadcastCompressedV2& f) {
                                    S = deserialize_block_candidate_broadcast(f, block_id, cc_seqno, validator_set_hash,
                                                                              data, max_decompressed_data_size);
                                  },
                                  [&](auto&) { S = td::Status::Error("unknown data type"); }));
  return S;
}

}  // namespace ton::validator::fullnode
