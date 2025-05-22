/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.
*/
#include "tolk.h"
#include "compiler-state.h"

/*
 *   This pipe finds unused symbols (global functions and variables) to strip them off codegen.
 *   It happens after converting AST to Op, so it does not traverse AST.
 *   In the future, when control flow graph is introduced, this should be done at AST level.
 */

namespace tolk {

static void mark_function_used_dfs(const std::unique_ptr<Op>& op);

static void mark_function_used(FunctionPtr fun_ref) {
  if (!fun_ref->is_code_function() || fun_ref->is_really_used()) { // already handled
    return;
  }

  fun_ref->mutate()->assign_is_really_used();
  mark_function_used_dfs(std::get<FunctionBodyCode*>(fun_ref->body)->code->ops);
}

static void mark_global_var_used(GlobalVarPtr glob_ref) {
  glob_ref->mutate()->assign_is_really_used();
}

static void mark_function_used_dfs(const std::unique_ptr<Op>& op) {
  if (!op) {
    return;
  }

  if (op->f_sym) {  // for Op::_Call
    mark_function_used(op->f_sym);
  }
  if (op->g_sym) {  // for Op::_GlobVar
    mark_global_var_used(op->g_sym);
  }
  mark_function_used_dfs(op->next);
  mark_function_used_dfs(op->block0);
  mark_function_used_dfs(op->block1);
}

void pipeline_find_unused_symbols() {
  for (FunctionPtr fun_ref : G.all_functions) {
    if (fun_ref->has_tvm_method_id()) {    // get methods, main and other entrypoints, regular functions with @method_id
      mark_function_used(fun_ref);
    }
  }
}

} // namespace tolk
