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

#include "ast.h"

/*
 *   This file contains a schema of aux_data inside ast_artificial_aux_vertex
 * (it's a compiler-inserted vertex that can't occur in source code).
 */

namespace tolk {

// AuxData_ForceFiftLocation is created when transforming AST to IR;
// it wraps constants to force codegen location point to usage, not to init_val AST nodes
struct AuxData_ForceFiftLocation final : ASTAuxData {
  SrcLocation forced_loc;

  explicit AuxData_ForceFiftLocation(SrcLocation forced_loc)
    : forced_loc(forced_loc) {
  }
};

} // namespace tolk
