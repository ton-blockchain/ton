/*
 * Copyright (c) 2024-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "auto/tl/ton_api.hpp"
#include "td/utils/lz4.h"
#include "td/utils/overloaded.h"
#include "tl-utils/tl-utils.hpp"
#include "vm/boc-compression.h"
#include "vm/boc.h"

#include "checksum.h"
#include "payload.h"

namespace ton::validator::consensus {

namespace {

td::Result<td::BufferSlice> compress_candidate_data(td::Slice block, td::Slice collated_data,
                                                    CandidateRootsRef decoded_roots) {
  std::vector<td::Ref<vm::Cell>> roots;
  if (!decoded_roots.is_null()) {
    auto block_file_hash = td::sha256_bits256(block);
    if (decoded_roots->block_file_hash != block_file_hash) {
      return td::Status::Error(PSTRING() << "candidate roots have invalid block file hash: declared "
                                         << decoded_roots->block_file_hash.to_hex() << ", actual "
                                         << block_file_hash.to_hex());
    }
    auto collated_file_hash = td::sha256_bits256(collated_data);
    if (decoded_roots->collated_file_hash != collated_file_hash) {
      return td::Status::Error(PSTRING() << "candidate roots have invalid collated data file hash: declared "
                                         << decoded_roots->collated_file_hash.to_hex() << ", actual "
                                         << collated_file_hash.to_hex());
    }
    roots.push_back(decoded_roots->block_root);
    roots.insert(roots.end(), decoded_roots->collated_roots.begin(), decoded_roots->collated_roots.end());
  } else {
    vm::BagOfCells boc1;
    TRY_STATUS(boc1.deserialize(block));
    if (boc1.get_root_count() != 1) {
      return td::Status::Error("block candidate should have exactly one root");
    }
    roots.push_back(boc1.get_root_cell());
    TRY_RESULT(collated_roots, vm::std_boc_deserialize_multi(collated_data, vm::BagOfCells::default_max_roots, true));
    roots.insert(roots.end(), collated_roots.begin(), collated_roots.end());
  }
  TRY_RESULT(compressed, vm::boc_compress(roots, vm::CompressionAlgorithm::ImprovedStructureLZ4));
  LOG(DEBUG) << "Compressing block candidate: " << block.size() + collated_data.size() << " -> " << compressed.size();
  return compressed;
}

td::Result<DeserializedPayload> decompress_candidate_data(td::Slice compressed, bool improved_compression,
                                                          int decompressed_size, int max_decompressed_size,
                                                          td::Bits256 root_hash, td::Bits256 src, int round) {
  std::vector<td::Ref<vm::Cell>> roots;
  if (!improved_compression) {
    TRY_RESULT(decompressed, td::lz4_decompress(compressed, decompressed_size));
    if (decompressed.size() != (size_t)decompressed_size) {
      return td::Status::Error("decompressed size mismatch");
    }
    TRY_RESULT_ASSIGN(roots, vm::std_boc_deserialize_multi(decompressed, vm::BagOfCells::default_max_roots, true));
  } else {
    TRY_RESULT_ASSIGN(roots, vm::boc_decompress(compressed, max_decompressed_size));
  }
  if (roots.empty()) {
    return td::Status::Error("boc is empty");
  }
  if (roots[0]->get_level() > 0) {
    return td::Status::Error("block root has nonzero level");
  }
  TRY_RESULT(block_data, vm::std_boc_serialize(roots[0], 31));
  auto block_root = std::move(roots[0]);
  roots.erase(roots.begin());
  TRY_RESULT(collated_data, vm::std_boc_serialize_multi(roots, 2));
  LOG(DEBUG) << "Decompressing block candidate " << (improved_compression ? "V2:" : ":") << compressed.size() << " -> "
             << block_data.size() + collated_data.size();
  auto decoded_roots = td::make_ref<CandidateRoots>(td::sha256_bits256(block_data), td::sha256_bits256(collated_data),
                                                    std::move(block_root), std::move(roots));
  return DeserializedPayload{
      create_tl_object<ton_api::validatorSession_candidate>(src, round, root_hash, std::move(block_data),
                                                            std::move(collated_data)),
      std::move(decoded_roots),
  };
}

}  // namespace

td::Result<td::BufferSlice> serialize_payload(const DeserializedPayload& block) {
  TRY_RESULT(compressed, compress_candidate_data(block.payload->data_, block.payload->collated_data_, block.roots));
  return create_serialize_tl_object<ton_api::validatorSession_compressedCandidateV2>(
      0, block.payload->src_, block.payload->round_, block.payload->root_hash_, std::move(compressed));
}

td::Result<DeserializedPayload> deserialize_payload(td::Slice data, int max_decompressed_data_size) {
  TRY_RESULT(f, fetch_tl_object<ton_api::validatorSession_Candidate>(data, true));
  td::Result<DeserializedPayload> res;
  ton_api::downcast_call(
      *f, td::overloaded(
              [&](ton_api::validatorSession_candidate& c) {
                res = td::Status::Error("Received decompressed tl object, while compression_enabled=true");
              },
              [&](ton_api::validatorSession_compressedCandidate& c) {
                res = [&]() -> td::Result<DeserializedPayload> {
                  if (c.decompressed_size_ > max_decompressed_data_size) {
                    return td::Status::Error("decompressed size is too big");
                  }
                  return decompress_candidate_data(c.data_, false, c.decompressed_size_, max_decompressed_data_size,
                                                   c.root_hash_, c.src_, c.round_);
                }();
              },
              [&](ton_api::validatorSession_compressedCandidateV2& c) {
                res = [&]() -> td::Result<DeserializedPayload> {
                  if (static_cast<int>(c.data_.size()) > max_decompressed_data_size) {
                    return td::Status::Error("Compressed data is too big");
                  }
                  return decompress_candidate_data(c.data_, true, 0, max_decompressed_data_size, c.root_hash_, c.src_,
                                                   c.round_);
                }();
              }));
  return res;
}

}  // namespace ton::validator::consensus
