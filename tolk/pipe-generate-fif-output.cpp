/*
    This file is part of TON Blockchain source code->

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

void FunctionBodyCode::set_code(CodeBlob* code) {
  this->code = code;
}

void FunctionBodyAsm::set_code(std::vector<AsmOp>&& code) {
  this->ops = std::move(code);
}


static void generate_output_func(FunctionPtr fun_ref) {
  tolk_assert(fun_ref->is_code_function());
  if (G.is_verbosity(2)) {
    std::cerr << "\n\n=========================\nfunction " << fun_ref->name << " : " << fun_ref->inferred_return_type << std::endl;
  }

  CodeBlob* code = std::get<FunctionBodyCode*>(fun_ref->body)->code;
  if (G.is_verbosity(3)) {
    code->print(std::cerr, 9);
  }
  code->prune_unreachable_code();
  if (G.is_verbosity(5)) {
    std::cerr << "after prune_unreachable: \n";
    code->print(std::cerr, 0);
  }
  for (int i = 0; i < 8; i++) {
    code->compute_used_code_vars();
    if (G.is_verbosity(4)) {
      std::cerr << "after compute_used_vars: \n";
      code->print(std::cerr, 6);
    }
    code->fwd_analyze();
    if (G.is_verbosity(5)) {
      std::cerr << "after fwd_analyze: \n";
      code->print(std::cerr, 6);
    }
    code->prune_unreachable_code();
    if (G.is_verbosity(5)) {
      std::cerr << "after prune_unreachable: \n";
      code->print(std::cerr, 6);
    }
  }
  code->mark_noreturn();
  if (G.is_verbosity(3)) {
    code->print(std::cerr, 15);
  }
  if (G.is_verbosity(2)) {
    std::cerr << "\n---------- resulting code for " << fun_ref->name << " -------------\n";
  }
  const char* modifier = "";
  if (fun_ref->is_inline()) {
    modifier = "INLINE";
  } else if (fun_ref->is_inline_ref()) {
    modifier = "REF";
  }
  if (G.settings.tolk_src_as_line_comments) {
    std::cout << "  // " << fun_ref->loc << std::endl;
  }
  std::cout << "  " << fun_ref->name << " PROC" << modifier << ":<{";
  int mode = 0;
  if (G.settings.stack_layout_comments) {
    mode |= Stack::_StackComments;
    size_t len = 2 + fun_ref->name.size() + 5 + std::strlen(modifier) + 3;
    while (len < 28) {      // a bit weird, but okay for now:
      std::cout << ' ';     // insert space after "xxx PROC" before `// stack state`
      len++;                // (the first AsmOp-comment that will be code generated)
    }                       // space is the same as used to align comments in asmops.cpp
    std::cout << '\t';
  } else {
    std::cout << std::endl;
  }
  if (G.settings.tolk_src_as_line_comments) {
    mode |= Stack::_LineComments;
  }
  if (fun_ref->is_inline() && code->ops->noreturn()) {
    mode |= Stack::_InlineFunc;
  }
  if (fun_ref->is_inline() || fun_ref->is_inline_ref()) {
    mode |= Stack::_InlineAny;
  }
  code->generate_code(std::cout, mode, 2);
  std::cout << "  " << "}>\n";
  if (G.is_verbosity(2)) {
    std::cerr << "--------------\n";
  }
}

void pipeline_generate_fif_output_to_std_cout() {
  std::cout << "\"Asm.fif\" include\n";
  std::cout << "// automatically generated from ";
  bool need_comma = false;
  for (const SrcFile* file : G.all_src_files) {
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
  for (FunctionPtr fun_ref : G.all_functions) {
    if (!fun_ref->does_need_codegen()) {
      if (G.is_verbosity(2) && fun_ref->is_code_function()) {
        std::cerr << fun_ref->name << ": code not generated, function does not need codegen\n";
      }
      continue;
    }

    if (fun_ref->is_entrypoint() && (fun_ref->name == "main" || fun_ref->name == "onInternalMessage")) {
      has_main_procedure = true;
    }

    std::cout << "  ";
    if (fun_ref->has_tvm_method_id()) {
      std::cout << fun_ref->tvm_method_id << " DECLMETHOD " << fun_ref->name << "\n";
    } else {
      std::cout << "DECLPROC " << fun_ref->name << "\n";
    }
  }

  if (!has_main_procedure) {
    throw Fatal("the contract has no entrypoint; forgot `fun onInternalMessage(...)`?");
  }

  for (GlobalVarPtr var_ref : G.all_global_vars) {
    if (!var_ref->is_really_used() && G.settings.remove_unused_functions) {
      if (G.is_verbosity(2)) {
        std::cerr << var_ref->name << ": variable not generated, it's unused\n";
      }
      continue;
    }

    std::cout << "  " << "DECLGLOBVAR " << var_ref->name << "\n";
  }

  for (FunctionPtr fun_ref : G.all_functions) {
    if (!fun_ref->does_need_codegen()) {
      continue;
    }
    generate_output_func(fun_ref);
  }

  std::cout << "}END>c\n";
  if (!G.settings.boc_output_filename.empty()) {
    std::cout << "boc>B \"" << G.settings.boc_output_filename << "\" B>file\n";
  }
}

} // namespace tolk
