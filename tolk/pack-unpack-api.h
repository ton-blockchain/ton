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

#include "pack-unpack-serializers.h"
#include "tolk.h"

namespace tolk {

bool check_struct_can_be_packed_or_unpacked(TypePtr any_type, bool is_pack, std::string& because_msg);

std::vector<var_idx_t> generate_pack_struct_to_cell(CodeBlob& code, SrcLocation loc, TypePtr any_type, std::vector<var_idx_t>&& ir_obj, const std::vector<var_idx_t>& ir_options);
std::vector<var_idx_t> generate_pack_struct_to_builder(CodeBlob& code, SrcLocation loc, TypePtr any_type, std::vector<var_idx_t>&& ir_builder, std::vector<var_idx_t>&& ir_obj, const std::vector<var_idx_t>& ir_options);
std::vector<var_idx_t> generate_unpack_struct_from_slice(CodeBlob& code, SrcLocation loc, TypePtr any_type, std::vector<var_idx_t>&& ir_slice, bool mutate_slice, const std::vector<var_idx_t>& ir_options);
std::vector<var_idx_t> generate_unpack_struct_from_cell(CodeBlob& code, SrcLocation loc, TypePtr any_type, std::vector<var_idx_t>&& ir_cell, const std::vector<var_idx_t>& ir_options);
std::vector<var_idx_t> generate_skip_struct_in_slice(CodeBlob& code, SrcLocation loc, TypePtr any_type, std::vector<var_idx_t>&& ir_slice, const std::vector<var_idx_t>& ir_options);

PackSize estimate_serialization_size(TypePtr any_type);
std::vector<var_idx_t> generate_estimate_size_call(CodeBlob& code, SrcLocation loc, TypePtr any_type);

} // namespace tolk
