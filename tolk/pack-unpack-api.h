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
PackSize estimate_serialization_size(TypePtr any_type);

// functions like T.toCell() are not declared here: they are implemented in a .cpp file,
// and their prototypes exist and are referenced in `builtins.cpp`

struct LazyStructLoadInfo;
struct LazyStructLoadedState;
struct LazyVariableLoadedState;

void generate_lazy_struct_from_slice(CodeBlob& code, AnyV origin, const LazyVariableLoadedState* lazy_variable, const LazyStructLoadInfo& load_info, const std::vector<var_idx_t>& ir_obj);
std::vector<var_idx_t> generate_lazy_struct_to_cell(CodeBlob& code, AnyV origin, const LazyStructLoadedState* loaded_state, std::vector<var_idx_t>&& ir_obj, const std::vector<var_idx_t>& ir_options);
std::vector<var_idx_t> generate_lazy_match_for_union(CodeBlob& code, AnyV origin, TypePtr union_type, const LazyVariableLoadedState* lazy_variable, const LazyMatchOptions& options);
std::vector<var_idx_t> generate_lazy_object_finish_loading(CodeBlob& code, AnyV origin, const LazyVariableLoadedState* lazy_variable, std::vector<var_idx_t>&& ir_obj);

} // namespace tolk
