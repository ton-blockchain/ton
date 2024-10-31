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
#include "lexer.h"
#include <getopt.h>
#include "ast-from-tokens.h"
#include "ast-to-legacy.h"
#include <fstream>
#include "td/utils/port/path.h"
#include <sys/stat.h>

namespace tolk {

// returns argument type of a function
// note, that when a function has multiple arguments, its arg type is a tensor (no arguments — an empty tensor)
// in other words, `f(int a, int b)` and `f((int,int) ab)` is the same when we speak about types
const TypeExpr *SymValFunc::get_arg_type() const {
  if (!sym_type)
    return nullptr;

  tolk_assert(sym_type->constr == TypeExpr::te_Map || sym_type->constr == TypeExpr::te_ForAll);
  const TypeExpr *te_map = sym_type->constr == TypeExpr::te_ForAll ? sym_type->args[0] : sym_type;
  const TypeExpr *arg_type = te_map->args[0];

  while (arg_type->constr == TypeExpr::te_Indirect) {
    arg_type = arg_type->args[0];
  }
  return arg_type;
}


bool SymValCodeFunc::does_need_codegen() const {
  // when a function is declared, but not referenced from code in any way, don't generate its body
  if (!is_really_used && G.pragma_remove_unused_functions.enabled()) {
    return false;
  }
  // when a function is referenced like `var a = some_fn;` (or in some other non-call way), its continuation should exist
  if (flags & flagUsedAsNonCall) {
    return true;
  }
  // when a function f() is just `return anotherF(...args)`, it doesn't need to be codegenerated at all,
  // since all its usages are inlined
  return !is_just_wrapper_for_another_f();
  // in the future, we may want to implement a true AST inlining for `inline` functions also
}

void mark_function_used_dfs(const std::unique_ptr<Op>& op);

void mark_function_used(SymValCodeFunc* func_val) {
  if (!func_val->code || func_val->is_really_used) { // already handled
    return;
  }

  func_val->is_really_used = true;
  mark_function_used_dfs(func_val->code->ops);
}

void mark_global_var_used(SymValGlobVar* glob_val) {
  glob_val->is_really_used = true;
}

void mark_function_used_dfs(const std::unique_ptr<Op>& op) {
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

void mark_used_symbols() {
  for (SymDef* func_sym : G.glob_func) {
    auto* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
    std::string name = G.symbols.get_name(func_sym->sym_idx);
    if (func_val->method_id.not_null() ||
        name == "main" || name == "recv_internal" || name == "recv_external" ||
        name == "run_ticktock" || name == "split_prepare" || name == "split_install") {
      mark_function_used(func_val);
    }
  }
}

/*
 *
 *   OUTPUT CODE GENERATOR
 *
 */

void generate_output_func(SymDef* func_sym) {
  SymValCodeFunc* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
  tolk_assert(func_val);
  std::string name = G.symbols.get_name(func_sym->sym_idx);
  if (G.is_verbosity(2)) {
    std::cerr << "\n\n=========================\nfunction " << name << " : " << func_val->get_type() << std::endl;
  }
  if (!func_val->code) {
    throw ParseError(func_sym->loc, "function `" + name + "` is just declared, not implemented");
  } else {
    CodeBlob& code = *(func_val->code);
    if (G.is_verbosity(3)) {
      code.print(std::cerr, 9);
    }
    code.simplify_var_types();
    if (G.is_verbosity(5)) {
      std::cerr << "after simplify_var_types: \n";
      code.print(std::cerr, 0);
    }
    code.prune_unreachable_code();
    if (G.is_verbosity(5)) {
      std::cerr << "after prune_unreachable: \n";
      code.print(std::cerr, 0);
    }
    code.split_vars(true);
    if (G.is_verbosity(5)) {
      std::cerr << "after split_vars: \n";
      code.print(std::cerr, 0);
    }
    for (int i = 0; i < 8; i++) {
      code.compute_used_code_vars();
      if (G.is_verbosity(4)) {
        std::cerr << "after compute_used_vars: \n";
        code.print(std::cerr, 6);
      }
      code.fwd_analyze();
      if (G.is_verbosity(5)) {
        std::cerr << "after fwd_analyze: \n";
        code.print(std::cerr, 6);
      }
      code.prune_unreachable_code();
      if (G.is_verbosity(5)) {
        std::cerr << "after prune_unreachable: \n";
        code.print(std::cerr, 6);
      }
    }
    code.mark_noreturn();
    if (G.is_verbosity(3)) {
      code.print(std::cerr, 15);
    }
    if (G.is_verbosity(2)) {
      std::cerr << "\n---------- resulting code for " << name << " -------------\n";
    }
    const char* modifier = "";
    if (func_val->is_inline()) {
      modifier = "INLINE";
    } else if (func_val->is_inline_ref()) {
      modifier = "REF";
    }
    std::cout << std::string(2, ' ') << name << " PROC" << modifier << ":<{\n";
    int mode = 0;
    if (G.settings.stack_layout_comments) {
      mode |= Stack::_StkCmt | Stack::_CptStkCmt;
    }
    if (func_val->is_inline() && code.ops->noreturn()) {
      mode |= Stack::_InlineFunc;
    }
    if (func_val->is_inline() || func_val->is_inline_ref()) {
      mode |= Stack::_InlineAny;
    }
    code.generate_code(std::cout, mode, 2);
    std::cout << std::string(2, ' ') << "}>\n";
    if (G.is_verbosity(2)) {
      std::cerr << "--------------\n";
    }
  }
}

// this function either throws or successfully prints whole program output to std::cout
void generate_output() {
  std::cout << "\"Asm.fif\" include\n";
  std::cout << "// automatically generated from " << G.generated_from << std::endl;
  std::cout << "PROGRAM{\n";
  mark_used_symbols();

  for (SymDef* func_sym : G.glob_func) {
    SymValCodeFunc* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
    tolk_assert(func_val);
    if (!func_val->does_need_codegen()) {
      if (G.is_verbosity(2)) {
        std::cerr << func_sym->name() << ": code not generated, function does not need codegen\n";
      }
      continue;
    }

    std::string name = G.symbols.get_name(func_sym->sym_idx);
    std::cout << std::string(2, ' ');
    if (func_val->method_id.is_null()) {
      std::cout << "DECLPROC " << name << "\n";
    } else {
      std::cout << func_val->method_id << " DECLMETHOD " << name << "\n";
    }
  }

  for (SymDef* gvar_sym : G.glob_vars) {
    auto* glob_val = dynamic_cast<SymValGlobVar*>(gvar_sym->value);
    tolk_assert(glob_val);
    if (!glob_val->is_really_used && G.pragma_remove_unused_functions.enabled()) {
      if (G.is_verbosity(2)) {
        std::cerr << gvar_sym->name() << ": variable not generated, it's unused\n";
      }
      continue;
    }
    std::string name = G.symbols.get_name(gvar_sym->sym_idx);
    std::cout << std::string(2, ' ') << "DECLGLOBVAR " << name << "\n";
  }

  for (SymDef* func_sym : G.glob_func) {
    SymValCodeFunc* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
    if (!func_val->does_need_codegen()) {
      continue;
    }
    generate_output_func(func_sym);
  }

  std::cout << "}END>c\n";
  if (!G.settings.boc_output_filename.empty()) {
    std::cout << "boc>B \"" << G.settings.boc_output_filename << "\" B>file\n";
  }
}


int tolk_proceed(const std::string &entrypoint_file_name) {
  define_builtins();
  lexer_init();
  G.pragma_allow_post_modification.always_on_and_deprecated("0.5.0");
  G.pragma_compute_asm_ltr.always_on_and_deprecated("0.5.0");

  try {
    {
      if (G.settings.stdlib_filename.empty()) {
        throw Fatal("stdlib filename not specified");
      }
      td::Result<SrcFile*> locate_res = locate_source_file(G.settings.stdlib_filename);
      if (locate_res.is_error()) {
        throw Fatal("Failed to locate stdlib: " + locate_res.error().message().str());
      }
      process_file_ast(parse_src_file_to_ast(locate_res.move_as_ok()));
    }
    td::Result<SrcFile*> locate_res = locate_source_file(entrypoint_file_name);
    if (locate_res.is_error()) {
      throw Fatal("Failed to locate " + entrypoint_file_name + ": " + locate_res.error().message().str());
    }
    process_file_ast(parse_src_file_to_ast(locate_res.move_as_ok()));

    // todo #ifdef TOLK_PROFILING + comment
    // lexer_measure_performance(all_src_files.get_all_files());

    generate_output();
    return 0;
  } catch (Fatal& fatal) {
    std::cerr << "fatal: " << fatal << std::endl;
    return 2;
  } catch (ParseError& error) {
    std::cerr << error << std::endl;
    return 2;
  } catch (UnifyError& unif_err) {
    std::cerr << "fatal: ";
    unif_err.print_message(std::cerr);
    std::cerr << std::endl;
    return 2;
  } catch (UnexpectedASTNodeType& error) {
    std::cerr << "fatal: " << error.what() << std::endl;
    std::cerr << "It's a compiler bug, please report to developers" << std::endl;
    return 2;
  }
}

}  // namespace tolk
