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
#include "td/utils/Time.h"
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

static td::Result<td::BufferSlice> serialize_block_broadcast_v2(const BlockBroadcast& broadcast,
                                                                std::string called_from) {
  size_t total_signatures_size = broadcast.sig_set->get_size() * 96;

  TRY_RESULT(data_root, vm::std_boc_deserialize(broadcast.data));

  auto t_compression_start = td::Time::now();
  vm::CompressionAlgorithm algorithm = vm::CompressionAlgorithm::ImprovedStructureLZ4;

  td::BufferSlice compressed_data;
  TRY_RESULT_ASSIGN(compressed_data, vm::boc_compress({data_root}, algorithm));
  size_t compressed_size = compressed_data.size();
  VLOG(FULL_NODE_DEBUG) << "Compressing block broadcast V2: "
                        << broadcast.data.size() + broadcast.proof.size() + total_signatures_size << " -> "
                        << compressed_data.size() + broadcast.proof.size() + total_signatures_size;
  auto res = create_serialize_tl_object<ton_api::tonNode_blockBroadcastCompressedV2>(
      create_tl_block_id(broadcast.block_id), broadcast.sig_set->tl(), 0, broadcast.proof.clone(),
      std::move(compressed_data));
  LOG(DEBUG) << "Broadcast_benchmark serialize_block_broadcast block_id=" << broadcast.block_id.to_str()
             << " called_from=" << called_from << " time_sec=" << (td::Time::now() - t_compression_start)
             << " compression=" << "compressed"
             << " original_size=" << broadcast.data.size() + broadcast.proof.size() + total_signatures_size
             << " compressed_size=" << compressed_size + broadcast.proof.size() + total_signatures_size;
  return res;
}

td::Result<td::BufferSlice> serialize_block_broadcast(const BlockBroadcast& broadcast, std::string called_from) {
  if (!broadcast.sig_set->is_ordinary()) {
    return serialize_block_broadcast_v2(broadcast, std::move(called_from));
  }
  std::vector<tl_object_ptr<ton_api::tonNode_blockSignature>> sigs = broadcast.sig_set->tl_legacy();
  size_t total_signatures_size = sigs.size() * 96;

  TRY_RESULT(proof_root, vm::std_boc_deserialize(broadcast.proof));
  TRY_RESULT(data_root, vm::std_boc_deserialize(broadcast.data));

  auto t_compression_start = td::Time::now();
  TRY_RESULT(boc, vm::std_boc_serialize_multi({proof_root, data_root}, 2));
  td::BufferSlice data =
      create_serialize_tl_object<ton_api::tonNode_blockBroadcastCompressed_data>(std::move(sigs), std::move(boc));
  td::BufferSlice compressed = td::lz4_compress(data);
  size_t compressed_size = compressed.size();
  VLOG(FULL_NODE_DEBUG) << "Compressing block broadcast: "
                        << broadcast.data.size() + broadcast.proof.size() + total_signatures_size << " -> "
                        << compressed.size();
  auto res = create_serialize_tl_object<ton_api::tonNode_blockBroadcastCompressed>(
      create_tl_block_id(broadcast.block_id), broadcast.sig_set->get_catchain_seqno(),
      broadcast.sig_set->get_validator_set_hash(), 0, std::move(compressed));
  LOG(DEBUG) << "Broadcast_benchmark serialize_block_broadcast block_id=" << broadcast.block_id.to_str()
             << " called_from=" << called_from << " time_sec=" << (td::Time::now() - t_compression_start)
             << " compression=" << "compressed"
             << " original_size=" << broadcast.data.size() + broadcast.proof.size() + total_signatures_size
             << " compressed_size=" << compressed_size;
  return res;
}

static td::Result<BlockBroadcast> deserialize_block_broadcast(ton_api::tonNode_blockBroadcast& f,
                                                              std::string called_from) {
  auto block_id = create_block_id(f.id_);
  auto t_decompression_start = td::Time::now();

  td::Ref<block::BlockSignatureSet> sig_set =
      block::BlockSignatureSet::fetch(f.signatures_, f.catchain_seqno_, f.validator_set_hash_);
  auto result = BlockBroadcast{block_id, std::move(sig_set), std::move(f.data_), std::move(f.proof_)};
  LOG(DEBUG) << "Broadcast_benchmark deserialize_block_broadcast block_id=" << block_id.to_str()
             << " called_from=" << called_from << " time_sec=" << (td::Time::now() - t_decompression_start)
             << " compression=" << "none"
             << " compressed_size=" << result.data.size() + result.proof.size() + f.signatures_.size() * 96;
  return result;
}

static td::Result<BlockBroadcast> deserialize_block_broadcast(ton_api::tonNode_blockBroadcastCompressed& f,
                                                              int max_decompressed_size, std::string called_from) {
  auto block_id = create_block_id(f.id_);
  auto t_decompression_start = td::Time::now();

  TRY_RESULT(decompressed, td::lz4_decompress(f.compressed_, max_decompressed_size));
  TRY_RESULT(f2, fetch_tl_object<ton_api::tonNode_blockBroadcastCompressed_data>(decompressed, true));
  td::Ref<block::BlockSignatureSet> sig_set =
      block::BlockSignatureSet::fetch(f2->signatures_, f.catchain_seqno_, f.validator_set_hash_);
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(f2->proof_data_, 2));
  if (roots.size() != 2) {
    return td::Status::Error("expected 2 roots in boc");
  }
  LOG(DEBUG) << "Broadcast_benchmark deserialize_block_broadcast block_id=" << block_id.to_str()
             << " called_from=" << called_from << " time_sec=" << (td::Time::now() - t_decompression_start)
             << " compression=" << "compressed"
             << " compressed_size=" << f.compressed_.size();
  TRY_RESULT(proof, vm::std_boc_serialize(roots[0], 0));
  TRY_RESULT(data, vm::std_boc_serialize(roots[1], 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block broadcast: " << f.compressed_.size() << " -> "
                        << data.size() + proof.size() + f2->signatures_.size() * 96;
  return BlockBroadcast{block_id, std::move(sig_set), std::move(data), std::move(proof)};
}

static td::Result<BlockBroadcast> deserialize_block_broadcast(ton_api::tonNode_blockBroadcastCompressedV2& f,
                                                              int max_decompressed_size, std::string called_from) {
  auto block_id = create_block_id(f.id_);
  auto t_decompression_start = td::Time::now();

  td::Ref<block::BlockSignatureSet> sig_set = block::BlockSignatureSet::fetch(f.signature_set_);
  size_t total_signatures_size = sig_set->get_size() * 96;
  TRY_RESULT(roots, vm::boc_decompress(f.data_compressed_, max_decompressed_size));
  if (roots.size() != 1) {
    return td::Status::Error("expected 1 root in boc");
  }
  LOG(DEBUG) << "Broadcast_benchmark deserialize_block_broadcast block_id=" << block_id.to_str()
             << " called_from=" << called_from << " time_sec=" << (td::Time::now() - t_decompression_start)
             << " compression=" << "compressedV2"
             << " compressed_size=" << f.data_compressed_.size() + f.proof_.size() + total_signatures_size;
  TRY_RESULT(data, vm::std_boc_serialize(roots[0], 31));
  return BlockBroadcast{create_block_id(f.id_), sig_set, std::move(data), std::move(f.proof_)};
}

td::Result<BlockBroadcast> deserialize_block_broadcast(ton_api::tonNode_Broadcast& obj, int max_decompressed_data_size,
                                                       std::string called_from) {
  td::Result<BlockBroadcast> B;
  ton_api::downcast_call(
      obj, td::overloaded([&](ton_api::tonNode_blockBroadcast& f) { B = deserialize_block_broadcast(f, called_from); },
                          [&](ton_api::tonNode_blockBroadcastCompressed& f) {
                            B = deserialize_block_broadcast(f, max_decompressed_data_size, called_from);
                          },
                          [&](ton_api::tonNode_blockBroadcastCompressedV2& f) {
                            B = deserialize_block_broadcast(f, max_decompressed_data_size, called_from);
                          },
                          [&](auto&) { B = td::Status::Error("unknown broadcast type"); }));
  return B;
}

td::Result<td::BufferSlice> serialize_block_full(const BlockIdExt& id, td::Slice proof, td::Slice data,
                                                 bool is_proof_link, bool compression_enabled) {
  if (!compression_enabled) {
    auto t_compression_start = td::Time::now();
    auto res = create_serialize_tl_object<ton_api::tonNode_dataFull>(create_tl_block_id(id), td::BufferSlice(proof),
                                                                     td::BufferSlice(data), is_proof_link);
    LOG(DEBUG) << "Broadcast_benchmark serialize_block_full block_id=" << id.to_str()
               << " time_sec=" << (td::Time::now() - t_compression_start) << " compression=" << "none"
               << " original_size=" << data.size() + proof.size() << " compressed_size=" << data.size() + proof.size();
    return res;
  }
  TRY_RESULT(proof_root, vm::std_boc_deserialize(proof));
  TRY_RESULT(data_root, vm::std_boc_deserialize(data));
  auto t_compression_start = td::Time::now();
  TRY_RESULT(boc, vm::std_boc_serialize_multi({proof_root, data_root}, 2));
  td::BufferSlice compressed = td::lz4_compress(boc);
  size_t compressed_size = compressed.size();
  VLOG(FULL_NODE_DEBUG) << "Compressing block full: " << data.size() + proof.size() << " -> " << compressed.size();
  auto res = create_serialize_tl_object<ton_api::tonNode_dataFullCompressed>(create_tl_block_id(id), 0,
                                                                             std::move(compressed), is_proof_link);
  LOG(DEBUG) << "Broadcast_benchmark serialize_block_full block_id=" << id.to_str()
             << " time_sec=" << (td::Time::now() - t_compression_start) << " compression=" << "compressed"
             << " original_size=" << data.size() + proof.size() << " compressed_size=" << compressed_size;
  return res;
}

static td::Status deserialize_block_full(ton_api::tonNode_dataFull& f, BlockIdExt& id, td::BufferSlice& proof,
                                         td::BufferSlice& data, bool& is_proof_link) {
  id = create_block_id(f.id_);
  auto t_decompression_start = td::Time::now();
  proof = std::move(f.proof_);
  data = std::move(f.block_);
  is_proof_link = f.is_link_;
  LOG(DEBUG) << "Broadcast_benchmark deserialize_block_full block_id=" << id.to_str()
             << " time_sec=" << (td::Time::now() - t_decompression_start) << " compression=" << "none"
             << " compressed_size=" << proof.size() + data.size();
  return td::Status::OK();
}

static td::Status deserialize_block_full(ton_api::tonNode_dataFullCompressed& f, BlockIdExt& id, td::BufferSlice& proof,
                                         td::BufferSlice& data, bool& is_proof_link, int max_decompressed_size) {
  id = create_block_id(f.id_);
  auto t_decompression_start = td::Time::now();

  TRY_RESULT(decompressed, td::lz4_decompress(f.compressed_, max_decompressed_size));
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed, 2));
  if (roots.size() != 2) {
    return td::Status::Error("expected 2 roots in boc");
  }
  LOG(DEBUG) << "Broadcast_benchmark deserialize_block_full block_id=" << id.to_str()
             << " time_sec=" << (td::Time::now() - t_decompression_start) << " compression=" << "compressed"
             << " compressed_size=" << f.compressed_.size();
  TRY_RESULT_ASSIGN(proof, vm::std_boc_serialize(roots[0], 0));
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(roots[1], 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block full: " << f.compressed_.size() << " -> " << data.size() + proof.size();
  is_proof_link = f.is_link_;
  return td::Status::OK();
}

static td::Status deserialize_block_full(ton_api::tonNode_dataFullCompressedV2& f, BlockIdExt& id,
                                         td::BufferSlice& proof, td::BufferSlice& data, bool& is_proof_link,
                                         int max_decompressed_size) {
  id = create_block_id(f.id_);
  auto t_decompression_start = td::Time::now();

  TRY_RESULT(roots, vm::boc_decompress(f.compressed_, max_decompressed_size));
  if (roots.size() != 2) {
    return td::Status::Error("expected 2 roots in boc");
  }
  LOG(DEBUG) << "Broadcast_benchmark deserialize_block_full block_id=" << id.to_str()
             << " time_sec=" << (td::Time::now() - t_decompression_start) << " compression=" << "compressedV2"
             << " compressed_size=" << f.compressed_.size();
  TRY_RESULT_ASSIGN(proof, vm::std_boc_serialize(roots[0], 0));
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(roots[1], 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block full V2: " << f.compressed_.size() << " -> "
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
                                                                td::uint32 validator_set_hash, td::Slice data,
                                                                bool compression_enabled, std::string called_from) {
  if (!compression_enabled) {
    auto t_compression_start = td::Time::now();
    auto res = create_serialize_tl_object<ton_api::tonNode_newBlockCandidateBroadcast>(
        create_tl_block_id(block_id), cc_seqno, validator_set_hash,
        create_tl_object<ton_api::tonNode_blockSignature>(Bits256::zero(), td::BufferSlice()), td::BufferSlice(data));
    LOG(DEBUG) << "Broadcast_benchmark serialize_block_candidate_broadcast block_id=" << block_id.to_str()
               << " called_from=" << called_from << " time_sec=" << (td::Time::now() - t_compression_start)
               << " compression=" << "none"
               << " original_size=" << data.size() << " compressed_size=" << data.size();
    return res;
  }
  TRY_RESULT(root, vm::std_boc_deserialize(data));
  auto t_compression_start = td::Time::now();
  TRY_RESULT(data_new, vm::std_boc_serialize(root, 2));
  td::BufferSlice compressed = td::lz4_compress(data_new);
  auto compressed_size = compressed.size();
  VLOG(FULL_NODE_DEBUG) << "Compressing block candidate broadcast: " << data.size() << " -> " << compressed_size;
  auto res = create_serialize_tl_object<ton_api::tonNode_newBlockCandidateBroadcastCompressed>(
      create_tl_block_id(block_id), cc_seqno, validator_set_hash,
      create_tl_object<ton_api::tonNode_blockSignature>(Bits256::zero(), td::BufferSlice()), 0, std::move(compressed));
  LOG(DEBUG) << "Broadcast_benchmark serialize_block_candidate_broadcast block_id=" << block_id.to_str()
             << " called_from=" << called_from << " time_sec=" << (td::Time::now() - t_compression_start)
             << " compression=" << "compressed"
             << " original_size=" << data.size() << " compressed_size=" << compressed_size;
  return res;
}

static td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_newBlockCandidateBroadcast& obj,
                                                        BlockIdExt& block_id, CatchainSeqno& cc_seqno,
                                                        td::uint32& validator_set_hash, td::BufferSlice& data,
                                                        std::string called_from) {
  auto t_decompression_start = td::Time::now();
  block_id = create_block_id(obj.id_);
  cc_seqno = obj.catchain_seqno_;
  validator_set_hash = obj.validator_set_hash_;
  data = std::move(obj.data_);
  LOG(DEBUG) << "Broadcast_benchmark deserialize_block_candidate_broadcast block_id=" << block_id.to_str()
             << " called_from=" << called_from << " time_sec=" << (td::Time::now() - t_decompression_start)
             << " compression=" << "none"
             << " compressed_size=" << data.size();
  return td::Status::OK();
}

static td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_newBlockCandidateBroadcastCompressed& obj,
                                                        BlockIdExt& block_id, CatchainSeqno& cc_seqno,
                                                        td::uint32& validator_set_hash, td::BufferSlice& data,
                                                        int max_decompressed_data_size, std::string called_from) {
  block_id = create_block_id(obj.id_);
  auto t_decompression_start = td::Time::now();
  cc_seqno = obj.catchain_seqno_;
  validator_set_hash = obj.validator_set_hash_;
  TRY_RESULT(decompressed, td::lz4_decompress(obj.compressed_, max_decompressed_data_size));
  TRY_RESULT(root, vm::std_boc_deserialize(decompressed));
  LOG(DEBUG) << "Broadcast_benchmark deserialize_block_candidate_broadcast block_id=" << block_id.to_str()
             << " called_from=" << called_from << " time_sec=" << (td::Time::now() - t_decompression_start)
             << " compression=" << "compressed"
             << " compressed_size=" << obj.compressed_.size();
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(root, 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block candidate broadcast: " << obj.compressed_.size() << " -> "
                        << data.size();
  return td::Status::OK();
}

static td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_newBlockCandidateBroadcastCompressedV2& obj,
                                                        BlockIdExt& block_id, CatchainSeqno& cc_seqno,
                                                        td::uint32& validator_set_hash, td::BufferSlice& data,
                                                        int max_decompressed_data_size, std::string called_from) {
  block_id = create_block_id(obj.id_);
  auto t_decompression_start = td::Time::now();
  cc_seqno = obj.catchain_seqno_;
  validator_set_hash = obj.validator_set_hash_;
  TRY_RESULT(roots, vm::boc_decompress(obj.compressed_, max_decompressed_data_size));
  if (roots.size() != 1) {
    return td::Status::Error("expected 1 root in boc");
  }
  LOG(DEBUG) << "Broadcast_benchmark deserialize_block_candidate_broadcast block_id=" << block_id.to_str()
             << " called_from=" << called_from << " time_sec=" << (td::Time::now() - t_decompression_start)
             << " compression=" << "compressedV2"
             << " compressed_size=" << obj.compressed_.size();
  auto root = std::move(roots[0]);
  TRY_RESULT_ASSIGN(data, vm::std_boc_serialize(root, 31));
  VLOG(FULL_NODE_DEBUG) << "Decompressing block candidate broadcast V2: " << obj.compressed_.size() << " -> "
                        << data.size();
  return td::Status::OK();
}

td::Status deserialize_block_candidate_broadcast(ton_api::tonNode_Broadcast& obj, BlockIdExt& block_id,
                                                 CatchainSeqno& cc_seqno, td::uint32& validator_set_hash,
                                                 td::BufferSlice& data, int max_decompressed_data_size,
                                                 std::string called_from) {
  td::Status S;
  ton_api::downcast_call(obj,
                         td::overloaded(
                             [&](ton_api::tonNode_newBlockCandidateBroadcast& f) {
                               S = deserialize_block_candidate_broadcast(f, block_id, cc_seqno, validator_set_hash,
                                                                         data, called_from);
                             },
                             [&](ton_api::tonNode_newBlockCandidateBroadcastCompressed& f) {
                               S = deserialize_block_candidate_broadcast(f, block_id, cc_seqno, validator_set_hash,
                                                                         data, max_decompressed_data_size, called_from);
                             },
                             [&](ton_api::tonNode_newBlockCandidateBroadcastCompressedV2& f) {
                               S = deserialize_block_candidate_broadcast(f, block_id, cc_seqno, validator_set_hash,
                                                                         data, max_decompressed_data_size, called_from);
                             },
                             [&](auto&) { S = td::Status::Error("unknown data type"); }));
  return S;
}

}  // namespace ton::validator::fullnode
