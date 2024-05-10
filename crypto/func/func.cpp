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

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "func.h"
#include "parser/srcread.h"
#include "parser/lexer.h"
#include <getopt.h>
#include "git.h"
#include <fstream>
#include "td/utils/port/path.h"

namespace funC {

int verbosity, indent, opt_level = 2;
bool stack_layout_comments, op_rewrite_comments, program_envelope, asm_preamble;
bool interactive = false;
GlobalPragma pragma_allow_post_modification{"allow-post-modification"};
GlobalPragma pragma_compute_asm_ltr{"compute-asm-ltr"};
GlobalPragma pragma_remove_unused_functions{"remove-unused-functions"};
std::string generated_from, boc_output_filename;
ReadCallback::Callback read_callback;

// returns argument type of a function
// note, that when a function has multiple arguments, its arg type is a tensor (no arguments — an empty tensor)
// in other words, `f(int a, int b)` and `f((int,int) ab)` is the same when we speak about types
const TypeExpr *SymValFunc::get_arg_type() const {
  if (!sym_type)
    return nullptr;

  func_assert(sym_type->constr == TypeExpr::te_Map || sym_type->constr == TypeExpr::te_ForAll);
  const TypeExpr *te_map = sym_type->constr == TypeExpr::te_ForAll ? sym_type->args[0] : sym_type;
  const TypeExpr *arg_type = te_map->args[0];

  while (arg_type->constr == TypeExpr::te_Indirect) {
    arg_type = arg_type->args[0];
  }
  return arg_type;
}


bool SymValCodeFunc::does_need_codegen() const {
  // when a function is declared, but not referenced from code in any way, don't generate its body
  if (!is_really_used && pragma_remove_unused_functions.enabled()) {
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

void GlobalPragma::enable(SrcLocation loc) {
  if (deprecated_from_v_) {
    loc.show_warning(PSTRING() << "#pragma " << name_ <<
                     " is deprecated since FunC v" << deprecated_from_v_ <<
                     ". Please, remove this line from your code.");
    return;
  }

  enabled_ = true;
  locs_.push_back(std::move(loc));
}

void GlobalPragma::check_enable_in_libs() {
  if (locs_.empty()) {
    return;
  }
  for (const SrcLocation& loc : locs_) {
    if (loc.fdescr->is_main) {
      return;
    }
  }
  locs_[0].show_warning(PSTRING() << "#pragma " << name_
                        << " is enabled in included libraries, it may change the behavior of your code. "
                        << "Add this #pragma to the main source file to suppress this warning.");
}

void GlobalPragma::always_on_and_deprecated(const char *deprecated_from_v) {
  deprecated_from_v_ = deprecated_from_v;
  enabled_ = true;
}

td::Result<std::string> fs_read_callback(ReadCallback::Kind kind, const char* query) {
  switch (kind) {
    case ReadCallback::Kind::ReadFile: {
      std::ifstream ifs{query};
      if (ifs.fail()) {
        auto msg = std::string{"cannot open source file `"} + query + "`";
        return td::Status::Error(msg);
      }
      std::stringstream ss;
      ss << ifs.rdbuf();
      return ss.str();
    }
    case ReadCallback::Kind::Realpath: {
      return td::realpath(td::CSlice(query));
    }
    default: {
      return td::Status::Error("Unknown query kind");
    }
  }
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
      func_assert(false);
    }
  }
  mark_function_used_dfs(op->next);
  mark_function_used_dfs(op->block0);
  mark_function_used_dfs(op->block1);
}

void mark_used_symbols() {
  for (SymDef* func_sym : glob_func) {
    auto* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
    std::string name = sym::symbols.get_name(func_sym->sym_idx);
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

void generate_output_func(SymDef* func_sym, std::ostream &outs, std::ostream &errs) {
  SymValCodeFunc* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
  func_assert(func_val);
  std::string name = sym::symbols.get_name(func_sym->sym_idx);
  if (verbosity >= 2) {
    errs << "\n\n=========================\nfunction " << name << " : " << func_val->get_type() << std::endl;
  }
  if (!func_val->code) {
    throw src::ParseError(func_sym->loc, "function `" + name + "` is just declared, not implemented");
  } else {
    CodeBlob& code = *(func_val->code);
    if (verbosity >= 3) {
      code.print(errs, 9);
    }
    code.simplify_var_types();
    if (verbosity >= 5) {
      errs << "after simplify_var_types: \n";
      code.print(errs, 0);
    }
    code.prune_unreachable_code();
    if (verbosity >= 5) {
      errs << "after prune_unreachable: \n";
      code.print(errs, 0);
    }
    code.split_vars(true);
    if (verbosity >= 5) {
      errs << "after split_vars: \n";
      code.print(errs, 0);
    }
    for (int i = 0; i < 8; i++) {
      code.compute_used_code_vars();
      if (verbosity >= 4) {
        errs << "after compute_used_vars: \n";
        code.print(errs, 6);
      }
      code.fwd_analyze();
      if (verbosity >= 5) {
        errs << "after fwd_analyze: \n";
        code.print(errs, 6);
      }
      code.prune_unreachable_code();
      if (verbosity >= 5) {
        errs << "after prune_unreachable: \n";
        code.print(errs, 6);
      }
    }
    code.mark_noreturn();
    if (verbosity >= 3) {
      code.print(errs, 15);
    }
    if (verbosity >= 2) {
      errs << "\n---------- resulting code for " << name << " -------------\n";
    }
    const char* modifier = "";
    if (func_val->is_inline()) {
      modifier = "INLINE";
    } else if (func_val->is_inline_ref()) {
      modifier = "REF";
    }
    outs << std::string(indent * 2, ' ') << name << " PROC" << modifier << ":<{\n";
    int mode = 0;
    if (stack_layout_comments) {
      mode |= Stack::_StkCmt | Stack::_CptStkCmt;
    }
    if (opt_level < 2) {
      mode |= Stack::_DisableOpt;
    }
    if (func_val->is_inline() && code.ops->noreturn()) {
      mode |= Stack::_InlineFunc;
    }
    if (func_val->is_inline() || func_val->is_inline_ref()) {
      mode |= Stack::_InlineAny;
    }
    code.generate_code(outs, mode, indent + 1);
    outs << std::string(indent * 2, ' ') << "}>\n";
    if (verbosity >= 2) {
      errs << "--------------\n";
    }
  }
}

int generate_output(std::ostream &outs, std::ostream &errs) {
  if (asm_preamble) {
    outs << "\"Asm.fif\" include\n";
  }
  outs << "// automatically generated from " << generated_from << std::endl;
  if (program_envelope) {
    outs << "PROGRAM{\n";
  }
  mark_used_symbols();
  for (SymDef* func_sym : glob_func) {
    SymValCodeFunc* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
    func_assert(func_val);
    if (!func_val->does_need_codegen()) {
      if (verbosity >= 2) {
        errs << func_sym->name() << ": code not generated, function does not need codegen\n";
      }
      continue;
    }

    std::string name = sym::symbols.get_name(func_sym->sym_idx);
    outs << std::string(indent * 2, ' ');
    if (func_val->method_id.is_null()) {
      outs << "DECLPROC " << name << "\n";
    } else {
      outs << func_val->method_id << " DECLMETHOD " << name << "\n";
    }
  }
  for (SymDef* gvar_sym : glob_vars) {
    auto* glob_val = dynamic_cast<SymValGlobVar*>(gvar_sym->value);
    func_assert(glob_val);
    if (!glob_val->is_really_used && pragma_remove_unused_functions.enabled()) {
      if (verbosity >= 2) {
        errs << gvar_sym->name() << ": variable not generated, it's unused\n";
      }
      continue;
    }
    std::string name = sym::symbols.get_name(gvar_sym->sym_idx);
    outs << std::string(indent * 2, ' ') << "DECLGLOBVAR " << name << "\n";
  }
  int errors = 0;
  for (SymDef* func_sym : glob_func) {
    SymValCodeFunc* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
    if (!func_val->does_need_codegen()) {
      continue;
    }
    try {
      generate_output_func(func_sym, outs, errs);
    } catch (src::Error& err) {
      errs << "cannot generate code for function `" << sym::symbols.get_name(func_sym->sym_idx) << "`:\n"
                << err << std::endl;
      ++errors;
    }
  }
  if (program_envelope) {
    outs << "}END>c\n";
  }
  if (!boc_output_filename.empty()) {
    outs << "2 boc+>B \"" << boc_output_filename << "\" B>file\n";
  }
  return errors;
}

void output_inclusion_stack(std::ostream &errs) {
  while (!funC::inclusion_locations.empty()) {
    src::SrcLocation loc = funC::inclusion_locations.top();
    funC::inclusion_locations.pop();
    if (loc.fdescr) {
      errs << "note: included from ";
      loc.show(errs);
      errs << std::endl;
    }
  }
}


int func_proceed(const std::vector<std::string> &sources, std::ostream &outs, std::ostream &errs) {
  if (funC::program_envelope && !funC::indent) {
    funC::indent = 1;
  }

  funC::define_keywords();
  funC::define_builtins();

  int ok = 0, proc = 0;
  try {
    for (auto src : sources) {
      ok += funC::parse_source_file(src.c_str(), {}, true);
      proc++;
    }
    if (funC::interactive) {
      funC::generated_from += "stdin ";
      ok += funC::parse_source_stdin();
      proc++;
    }
    if (ok < proc) {
      throw src::Fatal{"output code generation omitted because of errors"};
    }
    if (!proc) {
      throw src::Fatal{"no source files, no output"};
    }
    pragma_allow_post_modification.check_enable_in_libs();
    pragma_compute_asm_ltr.check_enable_in_libs();
    pragma_remove_unused_functions.check_enable_in_libs();
    return funC::generate_output(outs, errs);
  } catch (src::Fatal& fatal) {
    errs << "fatal: " << fatal << std::endl;
    output_inclusion_stack(errs);
    return 2;
  } catch (src::Error& error) {
    errs << error << std::endl;
    output_inclusion_stack(errs);
    return 2;
  } catch (funC::UnifyError& unif_err) {
    errs << "fatal: ";
    unif_err.print_message(errs);
    errs << std::endl;
    output_inclusion_stack(errs);
    return 2;
  }

  return 0;
}

}  // namespace funC
