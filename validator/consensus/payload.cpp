/*
 * Copyright (c) 2024-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "auto/tl/ton_api.hpp"
#include "td/utils/Time.h"
#include "td/utils/lz4.h"
#include "td/utils/overloaded.h"
#include "tl-utils/tl-utils.hpp"
#include "vm/boc-compression.h"
#include "vm/boc.h"

#include "payload.h"

namespace ton::validator::consensus {

namespace {

constexpr int VERBOSITY_NAME(VALIDATOR_SESSION_BENCHMARK) = verbosity_WARNING;

constexpr const char* k_called_from_validator_session = "validator_session";

td::Result<td::BufferSlice> compress_candidate_data(td::Slice block, td::Slice collated_data, size_t& decompressed_size,
                                                    std::string called_from, td::Bits256 root_hash) {
  vm::BagOfCells boc1;
  TRY_STATUS(boc1.deserialize(block));
  if (boc1.get_root_count() != 1) {
    return td::Status::Error("block candidate should have exactly one root");
  }
  std::vector<td::Ref<vm::Cell>> roots = {boc1.get_root_cell()};
  TRY_RESULT(collated_roots, vm::std_boc_deserialize_multi(collated_data));
  roots.insert(roots.end(), collated_roots.begin(), collated_roots.end());
  auto t_compression_start = td::Time::now();
  TRY_RESULT(data, vm::std_boc_serialize_multi(std::move(roots), 2));
  decompressed_size = data.size();
  td::BufferSlice compressed = td::lz4_compress(data);
  LOG(DEBUG) << "Compressing block candidate: " << block.size() + collated_data.size() << " -> " << compressed.size();
  VLOG(VALIDATOR_SESSION_BENCHMARK) << "Broadcast_benchmark serialize_candidate block_id=" << root_hash.to_hex()
                                    << " called_from=" << called_from
                                    << " time_sec=" << (td::Time::now() - t_compression_start)
                                    << " compression=" << "compressed"
                                    << " original_size=" << block.size() + collated_data.size()
                                    << " compressed_size=" << compressed.size();
  return compressed;
}

td::Result<std::pair<td::BufferSlice, td::BufferSlice>> decompress_candidate_data(
    td::Slice compressed, bool improved_compression, int decompressed_size, int max_decompressed_size,
    std::string called_from, td::Bits256 root_hash) {
  std::vector<td::Ref<vm::Cell>> roots;
  auto t_decompression_start = td::Time::now();
  if (!improved_compression) {
    TRY_RESULT(decompressed, td::lz4_decompress(compressed, decompressed_size));
    if (decompressed.size() != (size_t)decompressed_size) {
      return td::Status::Error("decompressed size mismatch");
    }
    TRY_RESULT_ASSIGN(roots, vm::std_boc_deserialize_multi(decompressed));
    VLOG(VALIDATOR_SESSION_BENCHMARK) << "Broadcast_benchmark deserialize_candidate block_id=" << root_hash.to_hex()
                                      << " called_from=" << called_from
                                      << " time_sec=" << (td::Time::now() - t_decompression_start)
                                      << " compression=" << "compressed" << " compressed_size=" << compressed.size();
  } else {
    TRY_RESULT_ASSIGN(roots, vm::boc_decompress(compressed, max_decompressed_size));
    TRY_RESULT(algorithm_name, vm::boc_get_algorithm_name(compressed));
    VLOG(VALIDATOR_SESSION_BENCHMARK) << "Broadcast_benchmark deserialize_candidate block_id=" << root_hash.to_hex()
                                      << " called_from=" << called_from
                                      << " time_sec=" << (td::Time::now() - t_decompression_start)
                                      << " compression=" << "compressedV2_" << algorithm_name
                                      << " compressed_size=" << compressed.size();
  }
  if (roots.empty()) {
    return td::Status::Error("boc is empty");
  }
  TRY_RESULT(block_data, vm::std_boc_serialize(roots[0], 31));
  roots.erase(roots.begin());
  TRY_RESULT(collated_data, vm::std_boc_serialize_multi(std::move(roots), 2));
  LOG(DEBUG) << "Decompressing block candidate " << (improved_compression ? "V2:" : ":") << compressed.size() << " -> "
             << block_data.size() + collated_data.size();
  return std::make_pair(std::move(block_data), std::move(collated_data));
}

}  // namespace

td::Result<td::BufferSlice> serialize_payload(const tl_object_ptr<ton_api::validatorSession_candidate>& block) {
  size_t decompressed_size;
  TRY_RESULT(compressed, compress_candidate_data(block->data_, block->collated_data_, decompressed_size,
                                                 k_called_from_validator_session, block->root_hash_))
  return create_serialize_tl_object<ton_api::validatorSession_compressedCandidate>(
      0, block->src_, block->round_, block->root_hash_, (int)decompressed_size, std::move(compressed));
}

td::Result<tl_object_ptr<ton_api::validatorSession_candidate>> deserialize_payload(td::Slice data,
                                                                                   int max_decompressed_data_size) {
  TRY_RESULT(f, fetch_tl_object<ton_api::validatorSession_Candidate>(data, true));
  td::Result<tl_object_ptr<ton_api::validatorSession_candidate>> res;
  ton_api::downcast_call(
      *f, td::overloaded(
              [&](ton_api::validatorSession_candidate& c) {
                res = td::Status::Error("Received decompressed tl object, while compression_enabled=true");
              },
              [&](ton_api::validatorSession_compressedCandidate& c) {
                res = [&]() -> td::Result<tl_object_ptr<ton_api::validatorSession_candidate>> {
                  if (c.decompressed_size_ > max_decompressed_data_size) {
                    return td::Status::Error("decompressed size is too big");
                  }
                  TRY_RESULT(p,
                             decompress_candidate_data(c.data_, false, c.decompressed_size_, max_decompressed_data_size,
                                                       k_called_from_validator_session, c.root_hash_));
                  return create_tl_object<ton_api::validatorSession_candidate>(c.src_, c.round_, c.root_hash_,
                                                                               std::move(p.first), std::move(p.second));
                }();
              },
              [&](ton_api::validatorSession_compressedCandidateV2& c) {
                res = [&]() -> td::Result<tl_object_ptr<ton_api::validatorSession_candidate>> {
                  if (static_cast<int>(c.data_.size()) > max_decompressed_data_size) {
                    return td::Status::Error("Compressed data is too big");
                  }
                  TRY_RESULT(p, decompress_candidate_data(c.data_, true, 0, max_decompressed_data_size,
                                                          k_called_from_validator_session, c.root_hash_));
                  return create_tl_object<ton_api::validatorSession_candidate>(c.src_, c.round_, c.root_hash_,
                                                                               std::move(p.first), std::move(p.second));
                }();
              }));
  return res;
}

}  // namespace ton::validator::consensus
