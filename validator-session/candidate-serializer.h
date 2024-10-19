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

namespace ton::validatorsession {

td::Result<td::BufferSlice> serialize_candidate(const tl_object_ptr<ton_api::validatorSession_candidate>& block,
                                                bool compression_enabled);
td::Result<tl_object_ptr<ton_api::validatorSession_candidate>> deserialize_candidate(td::Slice data,
                                                                                     bool compression_enabled,
                                                                                     int max_decompressed_data_size);

td::Result<td::BufferSlice> compress_candidate_data(td::Slice block, td::Slice collated_data,
                                                    size_t& decompressed_size);
td::Result<std::pair<td::BufferSlice, td::BufferSlice>> decompress_candidate_data(td::Slice compressed,
                                                                                  int decompressed_size);

}  // namespace ton::validatorsession
