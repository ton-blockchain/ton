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
#include "candidate-serializer.h"
#include "tl-utils/tl-utils.hpp"
#include "vm/boc.h"
#include "td/utils/lz4.h"
#include "validator-session-types.h"

namespace ton::validatorsession {

td::Result<td::BufferSlice> serialize_candidate(const tl_object_ptr<ton_api::validatorSession_candidate>& block,
                                                bool compression_enabled) {
  if (!compression_enabled) {
    return serialize_tl_object(block, true);
  }
  size_t decompressed_size;
  TRY_RESULT(compressed, compress_candidate_data(block->data_, block->collated_data_, decompressed_size))
  return create_serialize_tl_object<ton_api::validatorSession_compressedCandidate>(
      0, block->src_, block->round_, block->root_hash_, (int)decompressed_size, std::move(compressed));
}

td::Result<tl_object_ptr<ton_api::validatorSession_candidate>> deserialize_candidate(td::Slice data,
                                                                                     bool compression_enabled,
                                                                                     int max_decompressed_data_size) {
  if (!compression_enabled) {
    return fetch_tl_object<ton_api::validatorSession_candidate>(data, true);
  }
  TRY_RESULT(f, fetch_tl_object<ton_api::validatorSession_compressedCandidate>(data, true));
  if (f->decompressed_size_ > max_decompressed_data_size) {
    return td::Status::Error("decompressed size is too big");
  }
  TRY_RESULT(p, decompress_candidate_data(f->data_, f->decompressed_size_));
  return create_tl_object<ton_api::validatorSession_candidate>(f->src_, f->round_, f->root_hash_, std::move(p.first),
                                                               std::move(p.second));
}

td::Result<td::BufferSlice> compress_candidate_data(td::Slice block, td::Slice collated_data,
                                                    size_t& decompressed_size) {
  vm::BagOfCells boc1, boc2;
  TRY_STATUS(boc1.deserialize(block));
  if (boc1.get_root_count() != 1) {
    return td::Status::Error("block candidate should have exactly one root");
  }
  std::vector<td::Ref<vm::Cell>> roots = {boc1.get_root_cell()};
  TRY_STATUS(boc2.deserialize(collated_data));
  for (int i = 0; i < boc2.get_root_count(); ++i) {
    roots.push_back(boc2.get_root_cell(i));
  }
  TRY_RESULT(data, vm::std_boc_serialize_multi(std::move(roots), 2));
  decompressed_size = data.size();
  td::BufferSlice compressed = td::lz4_compress(data);
  LOG(DEBUG) << "Compressing block candidate: " << block.size() + collated_data.size() << " -> " << compressed.size();
  return compressed;
}

td::Result<std::pair<td::BufferSlice, td::BufferSlice>> decompress_candidate_data(td::Slice compressed,
                                                                                  int decompressed_size) {
  TRY_RESULT(decompressed, td::lz4_decompress(compressed, decompressed_size));
  if (decompressed.size() != (size_t)decompressed_size) {
    return td::Status::Error("decompressed size mismatch");
  }
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed));
  if (roots.empty()) {
    return td::Status::Error("boc is empty");
  }
  TRY_RESULT(block_data, vm::std_boc_serialize(roots[0], 31));
  roots.erase(roots.begin());
  TRY_RESULT(collated_data, vm::std_boc_serialize_multi(std::move(roots), 31));
  LOG(DEBUG) << "Decompressing block candidate: " << compressed.size() << " -> "
             << block_data.size() + collated_data.size();
  return std::make_pair(std::move(block_data), std::move(collated_data));
}

}  // namespace ton::validatorsession
