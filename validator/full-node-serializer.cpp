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
#include "full-node-serializer.hpp"
#include "ton/ton-tl.hpp"
#include "tl-utils/common-utils.hpp"
#include "auto/tl/ton_api.hpp"
#include "tl-utils/tl-utils.hpp"
#include "vm/boc.h"
#include "vm/boc-compression.h"
#include "td/utils/lz4.h"
#include "full-node.h"
#include "td/utils/overloaded.h"

namespace ton::validator::fullnode {

td::Result<td::BufferSlice> serialize_block_broadcast(const BlockBroadcast& broadcast, bool compression_enabled) {
  std::vector<tl_object_ptr<ton_api::tonNode_blockSignature>> sigs;
  for (auto& sig : broadcast.signatures) {
    sigs.emplace_back(create_tl_object<ton_api::tonNode_blockSignature>(sig.node, sig.signature.clone()));
  }
  if (!compression_enabled) {
    return create_serialize_tl_object<ton_api::tonNode_blockBroadcast>(
        create_tl_block_id(broadcast.block_id), broadcast.catchain_seqno, broadcast.validator_set_hash, std::move(sigs),
        broadcast.proof.clone(), broadcast.data.clone());
  }

  TRY_RESULT(proof_root, vm::std_boc_deserialize(broadcast.proof));
  TRY_RESULT(data_root, vm::std_boc_deserialize(broadcast.data));
  TRY_RESULT(compressed_boc, vm::boc_compress({proof_root, data_root}, vm::CompressionAlgorithm::ImprovedStructureLZ4));
  td::BufferSlice data =
      create_serialize_tl_object<ton_api::tonNode_blockBroadcastCompressed_data>(std::move(sigs), std::move(compressed_boc));
  VLOG(FULL_NODE_DEBUG) << "Compressing block broadcast: "
                        << broadcast.data.size() + broadcast.proof.size() + broadcast.signatures.size() * 96 << " -> "
                        << data.size();
  return create_serialize_tl_object<ton_api::tonNode_blockBroadcastCompressed>(
      create_tl_block_id(broadcast.block_id), broadcast.catchain_seqno, broadcast.validator_set_hash, 0,
      std::move(data));
}

static td::Result<BlockBroadcast> deserialize_block_broadcast(ton_api::tonNode_blockBroadcast& f) {
  std::vector<BlockSignature> signatures;
  for (auto& sig : f.signatures_) {
    signatures.emplace_back(BlockSignature{sig->who_, std::move(sig->signature_)});
  }
  return BlockBroadcast{create_block_id(f.id_),
                        std::move(signatures),
                        static_cast<UnixTime>(f.catchain_seqno_),
                        static_cast<td::uint32>(f.validator_set_hash_),
                        std::move(f.data_),
                        std::move(f.proof_)};
}

static td::Result<BlockBroadcast> deserialize_block_broadcast(ton_api::tonNode_blockBroadcastCompressed& f,
                                                              int max_decompressed_size) {
  TRY_RESULT(decompressed, td::lz4_decompress(f.compressed_, max_decompressed_size));
  TRY_RESULT(f2, fetch_tl_object<ton_api::tonNode_blockBroadcastCompressed_data>(decompressed, true));
  std::vector<BlockSignature> signatures;
  for (auto& sig : f2->signatures_) {
    signatures.emplace_back(BlockSignature{sig->who_, std::move(sig->signature_)});
  }
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(f2->proof_data_, 2));
  if (roots.size() != 2) {
    return td::Status::Error("expected 2 roots in boc");
  }
  TRY_RESULT(proof, vm::std_boc_serialize(roots[0], 0));
  TRY_RESULT(data, vm::std_boc_serialize(roots[1], 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block broadcast: " << f.compressed_.size() << " -> "
                        << data.size() + proof.size() + signatures.size() * 96;
  return BlockBroadcast{create_block_id(f.id_),
                        std::move(signatures),
                        static_cast<UnixTime>(f.catchain_seqno_),
                        static_cast<td::uint32>(f.validator_set_hash_),
                        std::move(data),
                        std::move(proof)};
}

static td::Result<BlockBroadcast> deserialize_block_broadcast(ton_api::tonNode_blockBroadcastCompressedV2& f,
                                                              int max_decompressed_size) {
  TRY_RESULT(f2, fetch_tl_object<ton_api::tonNode_blockBroadcastCompressed_data>(f.compressed_, true));
  std::vector<BlockSignature> signatures;
  for (auto& sig : f2->signatures_) {
    signatures.emplace_back(BlockSignature{sig->who_, std::move(sig->signature_)});
  }
  TRY_RESULT(roots, vm::boc_decompress(f2->proof_data_));
  if (roots.size() != 2) {
    return td::Status::Error("expected 2 roots in boc");
  }
  TRY_RESULT(proof, vm::std_boc_serialize(roots[0], 0));
  TRY_RESULT(data, vm::std_boc_serialize(roots[1], 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block broadcast: " << f.compressed_.size() << " -> "
                        << data.size() + proof.size() + signatures.size() * 96;
  return BlockBroadcast{create_block_id(f.id_),
                        std::move(signatures),
                        static_cast<UnixTime>(f.catchain_seqno_),
                        static_cast<td::uint32>(f.validator_set_hash_),
                        std::move(data),
                        std::move(proof)};
}

td::Result<BlockBroadcast> deserialize_block_broadcast(ton_api::tonNode_Broadcast& obj,
                                                       int max_decompressed_data_size) {
  td::Result<BlockBroadcast> B;
  ton_api::downcast_call(obj,
                         td::overloaded([&](ton_api::tonNode_blockBroadcast& f) { B = deserialize_block_broadcast(f); },
                                        [&](ton_api::tonNode_blockBroadcastCompressed& f) {
                                          B = deserialize_block_broadcast(f, max_decompressed_data_size);
                                        },
                                        [&](ton_api::tonNode_blockBroadcastCompressedV2& f) {
                                          B = deserialize_block_broadcast(f, max_decompressed_data_size);
                                        },
                                        [&](auto&) { B = td::Status::Error("unknown broadcast type"); }));
  return B;
}

td::Result<td::BufferSlice> serialize_block_full(const BlockIdExt& id, td::Slice proof, td::Slice data,
                                                 bool is_proof_link, bool compression_enabled) {
  if (!compression_enabled) {
    return create_serialize_tl_object<ton_api::tonNode_dataFull>(create_tl_block_id(id), td::BufferSlice(proof),
                                                                 td::BufferSlice(data), is_proof_link);
  }
  TRY_RESULT(proof_root, vm::std_boc_deserialize(proof));
  TRY_RESULT(data_root, vm::std_boc_deserialize(data));
  TRY_RESULT(compressed, vm::boc_compress({proof_root, data_root}, vm::CompressionAlgorithm::ImprovedStructureLZ4));

  VLOG(FULL_NODE_DEBUG) << "Compressing block full: " << data.size() + proof.size() << " -> " << compressed.size();
  return create_serialize_tl_object<ton_api::tonNode_dataFullCompressed>(create_tl_block_id(id), 0,
                                                                         std::move(compressed), is_proof_link);
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
  TRY_RESULT(decompressed, td::lz4_decompress(f.compressed_, max_decompressed_size));
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed, 2));
  if (roots.size() != 2) {
    return td::Status::Error("expected 2 roots in boc");
  }
  TRY_RESULT_ASSIGN(proof, vm::std_boc_serialize(roots[0], 0));
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(roots[1], 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block full: " << f.compressed_.size() << " -> " << data.size() + proof.size();
  id = create_block_id(f.id_);
  is_proof_link = f.is_link_;
  return td::Status::OK();
}

static td::Status deserialize_block_full(ton_api::tonNode_dataFullCompressedV2& f, BlockIdExt& id, td::BufferSlice& proof,
                                         td::BufferSlice& data, bool& is_proof_link, int max_decompressed_size) {
  TRY_RESULT(roots, vm::boc_decompress(f.compressed_));
  if (roots.size() != 2) {
    return td::Status::Error("expected 2 roots in boc");
  }
  TRY_RESULT_ASSIGN(proof, vm::std_boc_serialize(roots[0], 0));
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(roots[1], 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block full V2: " << f.compressed_.size() << " -> " << data.size() + proof.size();
  id = create_block_id(f.id_);
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
                                                                td::uint32 validator_set_hash, td::Slice data,
                                                                bool compression_enabled) {
  if (!compression_enabled) {
    return create_serialize_tl_object<ton_api::tonNode_newBlockCandidateBroadcast>(
        create_tl_block_id(block_id), cc_seqno, validator_set_hash,
        create_tl_object<ton_api::tonNode_blockSignature>(Bits256::zero(), td::BufferSlice()), td::BufferSlice(data));
  }
  TRY_RESULT(root, vm::std_boc_deserialize(data));
  TRY_RESULT(compressed, vm::boc_compress({root}, vm::CompressionAlgorithm::ImprovedStructureLZ4));
  VLOG(FULL_NODE_DEBUG) << "Compressing block candidate broadcast: " << data.size() << " -> " << compressed.size();
  return create_serialize_tl_object<ton_api::tonNode_newBlockCandidateBroadcastCompressed>(
      create_tl_block_id(block_id), cc_seqno, validator_set_hash,
      create_tl_object<ton_api::tonNode_blockSignature>(Bits256::zero(), td::BufferSlice()), 0, std::move(compressed));
}

static td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_newBlockCandidateBroadcast& obj,
                                                        BlockIdExt& block_id, CatchainSeqno& cc_seqno,
                                                        td::uint32& validator_set_hash, td::BufferSlice& data) {
  block_id = create_block_id(obj.id_);
  cc_seqno = obj.catchain_seqno_;
  validator_set_hash = obj.validator_set_hash_;
  data = std::move(obj.data_);
  return td::Status::OK();
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
  VLOG(FULL_NODE_DEBUG) << "Decompressing block candidate broadcast: " << obj.compressed_.size() << " -> "
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
  TRY_RESULT(roots, vm::boc_decompress(obj.compressed_));
  if (roots.size() != 1) {
    return td::Status::Error("expected 1 root in boc");
  }
  auto root = std::move(roots[0]);
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(root, 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block candidate broadcast V2: " << obj.compressed_.size() << " -> "
                        << data.size();
  return td::Status::OK();
}

td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_Broadcast& obj, BlockIdExt& block_id,
                                                 CatchainSeqno& cc_seqno, td::uint32& validator_set_hash,
                                                 td::BufferSlice& data, int max_decompressed_data_size) {
  td::Status S;
  ton_api::downcast_call(obj, td::overloaded(
                                  [&](ton_api::tonNode_newBlockCandidateBroadcast& f) {
                                    S = deserialize_block_candidate_broadcast(f, block_id, cc_seqno, validator_set_hash,
                                                                              data);
                                  },
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
