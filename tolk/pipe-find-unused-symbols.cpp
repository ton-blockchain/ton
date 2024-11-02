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
#include "src-file.h"
#include "compiler-state.h"

/*
 *   Here we find unused symbols (global functions and variables) to strip them off codegen.
 *   Note, that currently it's implemented as a standalone step after AST has been transformed to legacy Expr/Op.
 * The reason why it's not done on AST level is that symbol resolving is done too late. For instance,
 * having `beginCell()` there is not enough information in AST whether if points to a global function
 * or it's a local variable application.
 *   In the future, this should be done on AST level.
 */

namespace tolk {

static void mark_function_used_dfs(const std::unique_ptr<Op>& op);

static void mark_function_used(SymValCodeFunc* func_val) {
  if (!func_val->code || func_val->is_really_used) { // already handled
    return;
  }

  func_val->is_really_used = true;
  mark_function_used_dfs(func_val->code->ops);
}

static void mark_global_var_used(SymValGlobVar* glob_val) {
  glob_val->is_really_used = true;
}

static void mark_function_used_dfs(const std::unique_ptr<Op>& op) {
  if (!op) {
    return;
  }
  // op->fun_ref, despite its name, may actually ref global var
  // note, that for non-calls, e.g. `var a = some_fn` (Op::_Let), some_fn is Op::_GlobVar
  // (in other words, fun_ref exists not only for direct Op::_Call, but for non-call references also)
  if (op->fun_ref) {
    if (auto* func_val = dynamic_cast<SymValCodeFunc*>(op->fun_ref->value)) {
      mark_function_used(func_val);
    } else if (auto* glob_val = dynamic_cast<SymValGlobVar*>(op->fun_ref->value)) {
      mark_global_var_used(glob_val);
    } else if (auto* asm_val = dynamic_cast<SymValAsmFunc*>(op->fun_ref->value)) {
    } else {
      tolk_assert(false);
    }
  }
  mark_function_used_dfs(op->next);
  mark_function_used_dfs(op->block0);
  mark_function_used_dfs(op->block1);
}

void pipeline_find_unused_symbols() {
  for (SymDef* func_sym : G.all_code_functions) {
    auto* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
    std::string name = G.symbols.get_name(func_sym->sym_idx);
    if (func_val->method_id.not_null() || func_val->is_entrypoint()) {
      mark_function_used(func_val);
    }
  }
}

} // namespace tolk
