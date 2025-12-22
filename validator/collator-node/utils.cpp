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
#include "keys/keys.hpp"
#include "ton/ton-tl.hpp"
#include "validator-session/candidate-serializer.h"

#include "checksum.h"
#include "utils.hpp"

namespace ton::validator {

constexpr const char* k_called_from_collator_node = "collator_node";

tl_object_ptr<ton_api::collatorNode_Candidate> serialize_candidate(const BlockCandidate& block, bool compress) {
  if (!compress) {
    auto t_compression_start = td::Time::now();
    auto res = create_tl_object<ton_api::collatorNode_candidate>(
        PublicKey{pubkeys::Ed25519{block.pubkey.as_bits256()}}.tl(), create_tl_block_id(block.id), block.data.clone(),
        block.collated_data.clone());
    LOG(DEBUG) << "Broadcast_benchmark serialize_candidate block_id=" << block.id.root_hash.to_hex()
               << " called_from=" << k_called_from_collator_node
               << " time_sec=" << (td::Time::now() - t_compression_start) << " compression=" << "none"
               << " original_size=" << block.data.size() + block.collated_data.size()
               << " compressed_size=" << block.data.size() + block.collated_data.size();
    return res;
  }
  size_t decompressed_size;
  td::BufferSlice compressed =
      validatorsession::compress_candidate_data(block.data, block.collated_data, decompressed_size,
                                                k_called_from_collator_node, block.id.root_hash)
          .move_as_ok();
  return create_tl_object<ton_api::collatorNode_compressedCandidate>(
      0, PublicKey{pubkeys::Ed25519{block.pubkey.as_bits256()}}.tl(), create_tl_block_id(block.id),
      (int)decompressed_size, std::move(compressed));
}

td::Result<BlockCandidate> deserialize_candidate(tl_object_ptr<ton_api::collatorNode_Candidate> f,
                                                 int max_decompressed_data_size) {
  td::Result<BlockCandidate> res;
  ton_api::downcast_call(
      *f, td::overloaded(
              [&](ton_api::collatorNode_candidate& c) {
                res = [&]() -> td::Result<BlockCandidate> {
                  auto t_decompression_start = td::Time::now();
                  auto hash = td::sha256_bits256(c.collated_data_);
                  auto key = PublicKey{c.source_};
                  if (!key.is_ed25519()) {
                    return td::Status::Error("invalid pubkey");
                  }
                  auto e_key = Ed25519_PublicKey{key.ed25519_value().raw()};
                  auto block_id = create_block_id(c.id_);
                  BlockCandidate res{e_key, block_id, hash, std::move(c.data_), std::move(c.collated_data_)};
                  LOG(DEBUG) << "Broadcast_benchmark deserialize_candidate block_id=" << block_id.root_hash.to_hex()
                             << " called_from=" << k_called_from_collator_node
                             << " time_sec=" << (td::Time::now() - t_decompression_start) << " compression=" << "none"
                             << " compressed_size=" << res.data.size() + res.collated_data.size();
                  return std::move(res);
                }();
              },
              [&](ton_api::collatorNode_compressedCandidate& c) {
                res = [&]() -> td::Result<BlockCandidate> {
                  if (c.decompressed_size_ <= 0) {
                    return td::Status::Error("invalid decompressed size");
                  }
                  if (c.decompressed_size_ > max_decompressed_data_size) {
                    return td::Status::Error("decompressed size is too big");
                  }
                  TRY_RESULT(p, validatorsession::decompress_candidate_data(
                                    c.data_, false, c.decompressed_size_, max_decompressed_data_size,
                                    k_called_from_collator_node, create_block_id(c.id_).root_hash));
                  auto collated_data_hash = td::sha256_bits256(p.second);
                  auto key = PublicKey{c.source_};
                  if (!key.is_ed25519()) {
                    return td::Status::Error("invalid pubkey");
                  }
                  auto e_key = Ed25519_PublicKey{key.ed25519_value().raw()};
                  return BlockCandidate{e_key, create_block_id(c.id_), collated_data_hash, std::move(p.first),
                                        std::move(p.second)};
                }();
              },
              [&](ton_api::collatorNode_compressedCandidateV2& c) {
                res = [&]() -> td::Result<BlockCandidate> {
                  TRY_RESULT(p, validatorsession::decompress_candidate_data(
                                    c.data_, true, 0, max_decompressed_data_size,
                                    k_called_from_collator_node, create_block_id(c.id_).root_hash));
                  auto collated_data_hash = td::sha256_bits256(p.second);
                  auto key = PublicKey{c.source_};
                  if (!key.is_ed25519()) {
                    return td::Status::Error("invalid pubkey");
                  }
                  auto e_key = Ed25519_PublicKey{key.ed25519_value().raw()};
                  return BlockCandidate{e_key, create_block_id(c.id_), collated_data_hash, std::move(p.first),
                                        std::move(p.second)};
                }();
              }));
  return res;
}

}  // namespace ton::validator