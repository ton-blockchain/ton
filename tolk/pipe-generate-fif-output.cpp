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
#include "ast.h"
#include "compiler-state.h"

namespace tolk {

bool SymValCodeFunc::does_need_codegen() const {
  // when a function is declared, but not referenced from code in any way, don't generate its body
  if (!is_really_used && G.settings.remove_unused_functions) {
    return false;
  }
  // when a function is referenced like `var a = some_fn;` (or in some other non-call way), its continuation should exist
  if (flags & flagUsedAsNonCall) {
    return true;
  }
  // currently, there is no inlining, all functions are codegenerated
  // (but actually, unused ones are later removed by Fift)
  // in the future, we may want to implement a true AST inlining for "simple" functions
  return true;
}

void SymValCodeFunc::set_code(CodeBlob* code) {
  this->code = code;
}

void SymValAsmFunc::set_code(std::vector<AsmOp> code) {
  this->ext_compile = make_ext_compile(std::move(code));
}


static void generate_output_func(SymDef* func_sym) {
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

void pipeline_generate_fif_output_to_std_cout(const AllSrcFiles& all_src_files) {
  std::cout << "\"Asm.fif\" include\n";
  std::cout << "// automatically generated from ";
  bool need_comma = false;
  for (const SrcFile* file : all_src_files) {
    if (!file->is_stdlib_file()) {
      if (need_comma) {
        std::cout << ", ";
      }
      std::cout << file->rel_filename;
      need_comma = true;
    }
  }
  std::cout << std::endl;
  std::cout << "PROGRAM{\n";

  bool has_main_procedure = false;
  for (SymDef* func_sym : G.all_code_functions) {
    SymValCodeFunc* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
    tolk_assert(func_val);
    if (!func_val->does_need_codegen()) {
      if (G.is_verbosity(2)) {
        std::cerr << func_sym->name() << ": code not generated, function does not need codegen\n";
      }
      continue;
    }

    std::string name = G.symbols.get_name(func_sym->sym_idx);
    if (func_val->is_entrypoint() && (name == "main" || name == "onInternalMessage")) {
      has_main_procedure = true;
    }

    std::cout << std::string(2, ' ');
    if (func_val->method_id.is_null()) {
      std::cout << "DECLPROC " << name << "\n";
    } else {
      std::cout << func_val->method_id << " DECLMETHOD " << name << "\n";
    }
  }

  if (!has_main_procedure) {
    throw Fatal("the contract has no entrypoint; forgot `fun onInternalMessage(...)`?");
  }

  for (SymDef* gvar_sym : G.all_global_vars) {
    auto* glob_val = dynamic_cast<SymValGlobVar*>(gvar_sym->value);
    tolk_assert(glob_val);
    if (!glob_val->is_really_used && G.settings.remove_unused_functions) {
      if (G.is_verbosity(2)) {
        std::cerr << gvar_sym->name() << ": variable not generated, it's unused\n";
      }
      continue;
    }
    std::string name = G.symbols.get_name(gvar_sym->sym_idx);
    std::cout << std::string(2, ' ') << "DECLGLOBVAR " << name << "\n";
  }

  for (SymDef* func_sym : G.all_code_functions) {
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

} // namespace tolk
