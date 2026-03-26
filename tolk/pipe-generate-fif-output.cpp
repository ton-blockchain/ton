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
#include "compiler-settings.h"
#include "type-system.h"

namespace tolk {

void FunctionBodyCode::set_code(CodeBlob* code) {
  this->code = code;
}

void FunctionBodyAsm::set_code(std::vector<AsmOp>&& code) {
  this->ops = std::move(code);
}

// If "print line comments" is true in settings, every asm instruction is preceded by an original line from Tolk sources.
// This helper prints the first line of SrcRange and tracks last line printed to avoid duplicates.
struct LineCommentsOutput {
  int last_start_line_no = 0;
  int last_end_line_no = 0;

  static void output_line(std::ostream& os, int indent, std::string_view line_str, int line_no, bool dots = false) {
    // trim some characters from start and end to see `else if (x)` not `} else if (x) {`
    int b = 0, e = static_cast<int>(line_str.size() - 1);
    while (std::isspace(line_str[b]) || line_str[b] == '}') {
      if (b < e) b++;
      else break;
    }
    while (std::isspace(line_str[e]) || line_str[e] == '{' || line_str[e] == ';' || line_str[e] == ',') {
      if (e > b) e--;
      else break;
    }

    if (b < e) {
      for (int i = 0; i < indent * 2; ++i) {
        os << ' ';
      }
      os << "// " << (dots ? "..." : "") << line_no << ": " << line_str.substr(b, e - b + 1) << std::endl;
    }
  }

  void output_first_line(std::ostream& os, int indent, SrcRange range) {
    SrcRange::DecodedRange d = range.decode_offsets();

    bool just_printed_start_line = false;
    if (d.start_line_no != last_start_line_no) {
      output_line(os, indent, d.start_line_str, d.start_line_no);
      just_printed_start_line = true;
    }
    last_start_line_no = d.start_line_no;
    last_end_line_no = std::max(last_end_line_no, d.start_line_no);

    if (d.end_line_no > last_end_line_no) {
      std::string_view end_text = d.end_line_str.substr(0, d.end_char_no - 1);
      output_line(os, indent, end_text, d.end_line_no, just_printed_start_line);
    }
    last_end_line_no = std::max(last_end_line_no, d.end_line_no);
  }
};

static void output_asm_code_for_fun(std::ostream& os, FunctionPtr fun_ref, std::vector<AsmOp>&& asm_code, bool print_stack_comments, bool print_line_comments) {
  tolk_assert(!asm_code.empty());
  const AsmOp& enter_comment = asm_code.front();    // a comment with stack layout when entering a function
  tolk_assert(enter_comment.is_comment());

  const char* modifier = "PROC";
  if (fun_ref->inline_mode == FunctionInlineMode::inlineViaFif) {
    modifier = "PROCINLINE";
  } else if (fun_ref->inline_mode == FunctionInlineMode::inlineRef) {
    modifier = "PROCREF";
  }
  if (print_line_comments) {
    os << "  // " << fun_ref->ident_anchor->range.stringify_start_location(false) << std::endl;
  }
  std::string fun_fift_name = CodeBlob::fift_name(fun_ref);
  os << "  " << fun_fift_name << " " << modifier << ":<{";
  if (print_stack_comments) {
    size_t len = 2 + fun_fift_name.size() + 1 + std::strlen(modifier) + 3;
    while (len < 28) {      // align "// stack state"
      os << ' ';
      len++;
    }
    os << "\t// " << enter_comment.op;
  }
  os << std::endl;

  int len = static_cast<int>(asm_code.size());
  int indent = 2;
  LineCommentsOutput line_output;

  for (int i = 0; i < len; ++i) {
    const AsmOp& op = asm_code[i];
    if (op.is_comment()) {
      continue;
    }

    tolk_assert(op.origin);
    SrcRange range = op.origin->range;
    // it's `}>ELSE<{` or similar, not actually an asm instruction
    bool need_line_comment = range.is_valid() && !op.op.starts_with("}>");

    if (need_line_comment && print_line_comments) {
      line_output.output_first_line(os, indent, range);
    }

    // there may be several consecutive stack comments reflecting permutations, e.g.
    // [ "10 PUSHINT", "// '1=10", "// a=10", "// b=10" ]
    // take the last one
    const AsmOp* fwd_comment_op = nullptr;
    for (int j = i + 1; j < len && asm_code[j].is_comment(); ++j) {
      fwd_comment_op = &asm_code[j];
    }

    bool show_comment = print_stack_comments && fwd_comment_op;
    indent -= op.op.starts_with("}>");
    op.output_to_fif(os, indent, show_comment);
    indent += op.op.ends_with("<{");

    if (show_comment) {   // leading slashes already printed with proper indentation
      os << fwd_comment_op->op;
    }
    os << std::endl;
  }
  tolk_assert(indent == 2);
  os << "  " << "}>\n";
}

static void generate_output_func(std::ostream& os, FunctionPtr fun_ref) {
  tolk_assert(fun_ref->is_code_function());
  if (G_settings.verbosity >= 2) {
    std::cerr << "\n\n=========================\nfunction " << fun_ref->name << " : " << fun_ref->inferred_return_type->as_human_readable() << std::endl;
  }

  CodeBlob* code = std::get<FunctionBodyCode*>(fun_ref->body)->code;
  if (G_settings.verbosity >= 3) {
    code->print(std::cerr, 0);
  }
  code->prune_unreachable_code();
  if (G_settings.verbosity >= 5) {
    std::cerr << "after prune_unreachable: \n";
    code->print(std::cerr, 0);
  }
  // fixed-point iteration: backward analysis may disable unused ops, forward analysis may discover
  // constant values, prune may remove unreachable branches — each can enable further optimizations;
  // in practice, convergence happens in 2-3 iterations, 8 is a safe upper bound
  for (int i = 0; i < 8; i++) {
    code->compute_used_code_vars();
    if (G_settings.verbosity >= 4) {
      std::cerr << "after compute_used_vars: \n";
      code->print(std::cerr, 6);
    }
    code->fwd_analyze();
    if (G_settings.verbosity >= 5) {
      std::cerr << "after fwd_analyze: \n";
      code->print(std::cerr, 6);
    }
    code->prune_unreachable_code();
    if (G_settings.verbosity >= 5) {
      std::cerr << "after prune_unreachable: \n";
      code->print(std::cerr, 6);
    }
  }
  code->mark_noreturn();
  if (G_settings.verbosity >= 3) {
    // code->print(std::cerr, 15);
  }
  if (G_settings.verbosity >= 2) {
    std::cerr << "\n---------- resulting code for " << fun_ref->name << " -------------\n";
  }
  int mode = 0;
  if (fun_ref->inline_mode == FunctionInlineMode::inlineViaFif && code->ops.is_noreturn()) {
    mode |= Stack::_InlineFunc;
  }
  if (fun_ref->inline_mode == FunctionInlineMode::inlineViaFif || fun_ref->inline_mode == FunctionInlineMode::inlineRef) {
    mode |= Stack::_InlineAny;
  }

  std::vector<AsmOp> asm_code = code->generate_asm_code(mode);
  if (G_settings.optimization_level >= 2) {
    asm_code = optimize_asm_code(std::move(asm_code));
  }
  output_asm_code_for_fun(
    os,
    fun_ref,
    std::move(asm_code),
    G_settings.stack_layout_comments,
    G_settings.tolk_src_as_line_comments
  );

  if (G_settings.verbosity >= 2) {
    std::cerr << "--------------\n";
  }
}

void pipeline_generate_fif_output(std::ostream& os) {
  os << "\"Asm.fif\" include\n";
  os << "// automatically generated from ";
  bool need_comma = false;
  for (const SrcFile* file : G.all_src_files) {
    if (!file->is_stdlib_file) {
      if (need_comma) {
        os << ", ";
      }
      os << file->extract_short_name();
      need_comma = true;
    }
  }
  os << std::endl;
  os << "PROGRAM{\n";

  bool has_main_procedure = false;
  int n_inlined_in_place = 0;
  std::vector<FunctionPtr> all_contract_getters;
  for (FunctionPtr fun_ref : G.all_functions) {
    if (fun_ref->is_asm_function() || !fun_ref->does_need_codegen()) {
      if (G_settings.verbosity >= 2 && fun_ref->is_code_function()) {
        std::cerr << fun_ref->name << ": code not generated, function does not need codegen\n";
      }
      n_inlined_in_place += fun_ref->is_inlined_in_place() && fun_ref->is_really_used();
      continue;
    }

    if (fun_ref->is_entrypoint() && (fun_ref->name == "main" || fun_ref->name == "onInternalMessage")) {
      has_main_procedure = true;
    }

    os << "  ";
    if (fun_ref->has_tvm_method_id()) {
      os << fun_ref->tvm_method_id << " DECLMETHOD " << CodeBlob::fift_name(fun_ref) << "\n";
    } else {
      os << "DECLPROC " << CodeBlob::fift_name(fun_ref) << "\n";
    }

    if (fun_ref->is_contract_getter()) {
      for (FunctionPtr other : all_contract_getters) {
        if (other->tvm_method_id == fun_ref->tvm_method_id) {
          err("GET methods hash collision: `{}` and `{}` produce the same method_id={}. Consider renaming one of these functions.", other, fun_ref, fun_ref->tvm_method_id).fire(fun_ref->ident_anchor);
        }
      }
      all_contract_getters.push_back(fun_ref);
    }
  }

  if (!has_main_procedure && !G_settings.allow_no_entrypoint) {
    throw Fatal("the contract has no entrypoint; forgot `fun onInternalMessage(...)`?");
  }

  if (n_inlined_in_place) {
    os << "  // " << n_inlined_in_place << " functions inlined in-place:" << "\n";
    for (FunctionPtr fun_ref : G.all_functions) {
      if (fun_ref->is_inlined_in_place()) {
        os << "  // - " << fun_ref->name << " (" << fun_ref->n_times_called << (fun_ref->n_times_called == 1 ? " call" : " calls") << ")\n";
      }
    }
  }

  for (GlobalVarPtr var_ref : G.all_global_vars) {
    if (!var_ref->is_really_used()) {
      if (G_settings.verbosity >= 2) {
        std::cerr << var_ref->name << ": variable not generated, it's unused\n";
      }
      continue;
    }

    os << "  " << "DECLGLOBVAR " << CodeBlob::fift_name(var_ref) << "\n";
  }

  for (FunctionPtr fun_ref : G.all_functions) {
    if (fun_ref->is_asm_function() || !fun_ref->does_need_codegen()) {
      continue;
    }
    generate_output_func(os, fun_ref);
  }

  os << "}END>c\n";
  if (!G_settings.boc_output_filename.empty()) {
    os << "boc>B \"" << G_settings.boc_output_filename << "\" B>file\n";
  }
}

} // namespace tolk
