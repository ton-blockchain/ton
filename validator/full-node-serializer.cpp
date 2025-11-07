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
#include "tl-utils/tl-utils.hpp"
#include "ton/ton-tl.hpp"
#include "vm/boc-compression.h"
#include "vm/boc.h"

#include "full-node-serializer.hpp"
#include "full-node.h"

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
  TRY_RESULT(boc, vm::std_boc_serialize_multi({proof_root, data_root}, 2));
  td::BufferSlice data =
      create_serialize_tl_object<ton_api::tonNode_blockBroadcastCompressed_data>(std::move(sigs), std::move(boc));
  td::BufferSlice compressed = td::lz4_compress(data);
  VLOG(FULL_NODE_DEBUG) << "Compressing block broadcast: "
                        << broadcast.data.size() + broadcast.proof.size() + broadcast.signatures.size() * 96 << " -> "
                        << compressed.size();
  return create_serialize_tl_object<ton_api::tonNode_blockBroadcastCompressed>(
      create_tl_block_id(broadcast.block_id), broadcast.catchain_seqno, broadcast.validator_set_hash, 0,
      std::move(compressed));
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
  std::vector<BlockSignature> signatures;
  for (auto& sig : f.signatures_) {
    signatures.emplace_back(BlockSignature{sig->who_, std::move(sig->signature_)});
  }
  TRY_RESULT(roots, vm::boc_decompress(f.compressed_, max_decompressed_size));
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
  TRY_RESULT(boc, vm::std_boc_serialize_multi({proof_root, data_root}, 2));
  td::BufferSlice compressed = td::lz4_compress(boc);
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
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed, max_collated_data_roots + 1, true));
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

static td::Status deserialize_block_full(ton_api::tonNode_dataFullCompressedV2& f, BlockIdExt& id,
                                         td::BufferSlice& proof, td::BufferSlice& data, bool& is_proof_link,
                                         int max_decompressed_size) {
  TRY_RESULT(roots, vm::boc_decompress(f.compressed_, max_decompressed_size));
  if (roots.size() != 2) {
    return td::Status::Error("expected 2 roots in boc");
  }
  TRY_RESULT_ASSIGN(proof, vm::std_boc_serialize(roots[0], 0));
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(roots[1], 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block full V2: " << f.compressed_.size() << " -> "
                        << data.size() + proof.size();
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
                                                                td::optional<td::Slice> collated_data) {
  TRY_RESULT(root, vm::std_boc_deserialize(data));
  std::vector<Ref<vm::Cell>> roots = {root};
  if (collated_data) {
    TRY_RESULT(collated_data_roots,
               vm::std_boc_deserialize_multi(collated_data.value(), max_collated_data_roots, true));
    roots.insert(roots.end(), collated_data_roots.begin(), collated_data_roots.end());
  }
  TRY_RESULT(data_new, vm::std_boc_serialize_multi(std::move(roots), 2));
  td::BufferSlice compressed = td::lz4_compress(data_new);
  VLOG(FULL_NODE_DEBUG) << "Compressing block candidate broadcast " << (collated_data ? "(with cdata)" : "(no cdata)")
                        << ": " << data.size() + (collated_data ? collated_data.value().size() : 0) << " -> "
                        << compressed.size();
  bool with_collated_data = (bool)collated_data;
  return create_serialize_tl_object<ton_api::tonNode_blockCandidateBroadcastCompressed>(
      create_tl_block_id(block_id), cc_seqno, validator_set_hash,
      create_tl_object<ton_api::tonNode_blockSignature>(Bits256::zero(), td::BufferSlice()), with_collated_data ? 1 : 0,
      std::move(compressed));
}

static td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_blockCandidateBroadcastCompressed& obj,
                                                        BlockIdExt& block_id, CatchainSeqno& cc_seqno,
                                                        td::uint32& validator_set_hash, td::BufferSlice& data,
                                                        td::optional<td::BufferSlice>& collated_data,
                                                        int max_decompressed_data_size) {
  block_id = create_block_id(obj.id_);
  cc_seqno = obj.catchain_seqno_;
  validator_set_hash = obj.validator_set_hash_;
  TRY_RESULT(decompressed, td::lz4_decompress(obj.compressed_, max_decompressed_data_size));
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed, max_collated_data_roots + 1, true));
  if (roots.empty()) {
    return td::Status::Error("expected at least 1 root in boc");
  }
  if (!(obj.flags_ & 1) && roots.size() != 1) {
    return td::Status::Error("expected 1 root in boc");
  }
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(roots[0], 31));
  if (obj.flags_ & 1) {
    roots.erase(roots.begin());
    TRY_RESULT_ASSIGN(collated_data, vm::std_boc_serialize_multi(std::move(roots), 2));
  } else {
    collated_data = {};
  }
  VLOG(FULL_NODE_DEBUG) << "Decompressing block candidate broadcast " << (collated_data ? "(with cdata)" : "(no cdata)")
                        << ": " << obj.compressed_.size() << " -> "
                        << data.size() + (collated_data ? collated_data.value().size() : 0);
  return td::Status::OK();
}

static td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_blockCandidateBroadcastCompressedV2& obj,
                                                        BlockIdExt& block_id, CatchainSeqno& cc_seqno,
                                                        td::uint32& validator_set_hash, td::BufferSlice& data,
                                                        td::optional<td::BufferSlice>& collated_data,
                                                        int max_decompressed_data_size) {
  block_id = create_block_id(obj.id_);
  cc_seqno = obj.catchain_seqno_;
  validator_set_hash = obj.validator_set_hash_;
  TRY_RESULT(roots, vm::boc_decompress(obj.compressed_, max_decompressed_data_size));
  if (roots.empty()) {
    return td::Status::Error("expected at least 1 root in boc");
  }
  if (!(obj.flags_ & 1) && roots.size() != 1) {
    return td::Status::Error("expected 1 root in boc");
  }
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(roots[0], 31));
  if (obj.flags_ & 1) {
    roots.erase(roots.begin());
    TRY_RESULT_ASSIGN(collated_data, vm::std_boc_serialize_multi(std::move(roots), 2));
  } else {
    collated_data = {};
  }
  VLOG(FULL_NODE_DEBUG) << "Decompressing block candidate broadcast V2 "
                        << (collated_data ? "(with cdata)" : "(no cdata)") << ": " << obj.compressed_.size() << " -> "
                        << data.size() + (collated_data ? collated_data.value().size() : 0);
  return td::Status::OK();
}

td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_Broadcast& obj, BlockIdExt& block_id,
                                                 CatchainSeqno& cc_seqno, td::uint32& validator_set_hash,
                                                 td::BufferSlice& data, td::optional<td::BufferSlice>& collated_data,
                                                 int max_decompressed_data_size) {
  td::Status S;
  ton_api::downcast_call(
      obj, td::overloaded(
               [&](ton_api::tonNode_blockCandidateBroadcastCompressed& f) {
                 S = deserialize_block_candidate_broadcast(f, block_id, cc_seqno, validator_set_hash, data,
                                                           collated_data, max_decompressed_data_size);
               },
               [&](ton_api::tonNode_blockCandidateBroadcastCompressedV2& f) {
                 S = deserialize_block_candidate_broadcast(f, block_id, cc_seqno, validator_set_hash, data,
                                                           collated_data, max_decompressed_data_size);
               },
               [&](auto&) { S = td::Status::Error("unknown data type"); }));
  return S;
}

td::Result<td::BufferSlice> serialize_block_candidate_data(BlockCandidate candidate, bool only_collated_data,
                                                           bool compression_enabled) {
  if (!compression_enabled) {
    return create_serialize_tl_object<ton_api::tonNode_blockCandidateData>(
        create_tl_block_id(candidate.id), only_collated_data ? td::BufferSlice{} : std::move(candidate.data),
        std::move(candidate.collated_data));
  }

  std::vector<Ref<vm::Cell>> roots;
  vm::BagOfCells boc1, boc2;
  if (!only_collated_data) {
    TRY_STATUS(boc1.deserialize(candidate.data));
    if (boc1.get_root_count() != 1) {
      return td::Status::Error("block should have exactly one root");
    }
    roots.push_back(boc1.get_root_cell());
  }
  TRY_STATUS(boc2.deserialize(candidate.collated_data));
  for (int i = 0; i < boc2.get_root_count(); ++i) {
    roots.push_back(boc2.get_root_cell(i));
  }
  td::BufferSlice compressed;
  if (!roots.empty()) {
    TRY_RESULT_ASSIGN(compressed, vm::boc_compress(roots, vm::CompressionAlgorithm::ImprovedStructureLZ4));
  }
  if (only_collated_data) {
    LOG(DEBUG) << "Compressing block candidate (only cdata): " << candidate.collated_data.size() << " -> "
               << compressed.size();
  } else {
    LOG(DEBUG) << "Compressing block candidate (full): " << candidate.data.size() + candidate.collated_data.size()
               << " -> " << compressed.size();
  }
  return create_serialize_tl_object<ton_api::tonNode_blockCandidateDataCompressedV2>(create_tl_block_id(candidate.id),
                                                                                     0, std::move(compressed));
}

td::Status deserialize_block_candidate_data(ton_api::tonNode_blockCandidateData& obj, bool only_collated_data,
                                            BlockIdExt& id, td::BufferSlice& data, td::BufferSlice& collated_data) {
  id = create_block_id(obj.block_id_);
  data = only_collated_data ? td::BufferSlice{} : std::move(obj.data_);
  collated_data = std::move(obj.collated_data_);
  return td::Status::OK();
}

td::Status deserialize_block_candidate_data(ton_api::tonNode_blockCandidateDataCompressedV2& obj,
                                            bool only_collated_data, BlockIdExt& id, td::BufferSlice& data,
                                            td::BufferSlice& collated_data, int max_decompressed_size) {
  std::vector<Ref<vm::Cell>> roots;
  if (!obj.compressed_.empty()) {
    TRY_RESULT_ASSIGN(roots, vm::boc_decompress(obj.compressed_, max_decompressed_size));
  }
  if (only_collated_data) {
    data = td::BufferSlice{};
  } else {
    if (roots.empty()) {
      return td::Status::Error("expected at least one root");
    }
    TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(roots[0], 31));
    roots.erase(roots.begin());
  }
  TRY_RESULT_ASSIGN(collated_data, vm::std_boc_serialize_multi(roots, 2));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block candidate V2 " << (only_collated_data ? "(only cdata)" : "(full)")
                        << ": " << obj.compressed_.size() << " -> " << data.size() + collated_data.size();
  id = create_block_id(obj.block_id_);
  return td::Status::OK();
}

td::Status deserialize_block_candidate_data(ton_api::tonNode_BlockCandidateData& obj, bool only_collated_data,
                                            BlockIdExt& id, td::BufferSlice& data, td::BufferSlice& collated_data,
                                            int max_decompressed_data_size) {
  td::Status S;
  ton_api::downcast_call(obj, td::overloaded(
                                  [&](ton_api::tonNode_blockCandidateData& f) {
                                    S = deserialize_block_candidate_data(f, only_collated_data, id, data,
                                                                         collated_data);
                                  },
                                  [&](ton_api::tonNode_blockCandidateDataCompressedV2& f) {
                                    S = deserialize_block_candidate_data(f, only_collated_data, id, data, collated_data,
                                                                         max_decompressed_data_size);
                                  },
                                  [&](auto&) { S = td::Status::Error("unknown data type"); }));
  return S;
}

}  // namespace ton::validator::fullnode
