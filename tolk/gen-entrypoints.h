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

#include "ast-aux-data.h"
#include "fwd-declarations.h"
#include "tolk.h"

namespace tolk {

void handle_onInternalMessage_codegen_start(FunctionPtr f_onInternalMessage, const std::vector<var_idx_t>& rvect_params, CodeBlob& code, AnyV origin);
std::vector<var_idx_t> generate_get_requested_field_parsing_on_demand(const AuxData_OnInternalMessage_getField* aux_data, CodeBlob& code, AnyV origin);

} // namespace tolk
