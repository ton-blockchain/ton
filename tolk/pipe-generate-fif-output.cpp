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

struct IRVarsList {
  const std::vector<TmpVar>& var_names;
  const std::vector<var_idx_t>& ir_idx_arr;
};

struct StackCommentInFif {
  const std::vector<DebugMarkCurrentStack::StackSlot>& stack_slots;
  bool always_show_ir_idx;
};

struct CommentForDebugMarkInFif {
  const std::vector<TmpVar>& var_names;
  const DebugMarkInfo& debug_mark;
};

static std::ostream& operator<<(std::ostream& os, const IRVarsList& ir_vars) {
  os << '[';
  for (var_idx_t var_idx : ir_vars.ir_idx_arr) {
    tolk_assert(var_idx < static_cast<int>(ir_vars.var_names.size()));
    os << ' ' << '\'' << var_idx;
    if (!ir_vars.var_names[var_idx].name.empty()) {
      os << '_' << ir_vars.var_names[var_idx].name;
    }
  }
  os << ' ' << ']';
  return os;
}

static std::ostream& operator<<(std::ostream& os, const SrcRange& range) {
  std::string_view substr = range.decode_offsets().text_inside;
  if (std::string::size_type nl = substr.find('\n'); nl != std::string::npos) {
    os << substr.substr(0, nl) << "...";
  } else {
    os << substr;
  }
  return os;
}

static std::ostream& operator<<(std::ostream& os, const StackCommentInFif& sc) {
  for (DebugMarkCurrentStack::StackSlot slot : sc.stack_slots) {
    if (sc.always_show_ir_idx) {
      os << ' ' << '\'' << slot.ir_var->ir_idx;
      if (!slot.ir_var->name.empty()) {
        os << '_' << slot.ir_var->name;
      } else {
#ifdef TOLK_DEBUG
        // uncomment for detailed stack output, like `'15(binary-op) '16(glob-var)`
        // if (tmp_var->purpose) os << tmp_var->purpose;
#endif
      }
    } else {
      if (!slot.ir_var->name.empty()) {
        os << ' ' << slot.ir_var->name;
      } else {
        os << ' ' << '\'' << slot.ir_var->ir_idx;
      }
    }
    if (!slot.int_val.is_null()) {
      os << '=' << slot.int_val->to_dec_string();
    }
  }

  return os;
}

static std::ostream& operator<<(std::ostream& os, const CommentForDebugMarkInFif& sc) {
  if (const auto* stack_info = std::get_if<DebugMarkCurrentStack>(&sc.debug_mark)) {
    os << '[' << StackCommentInFif(stack_info->stack_slots, true) << ' ' << ']';
  } else if (const auto* cur_loc = std::get_if<DebugMarkLocation>(&sc.debug_mark)) {
    os << '`' << cur_loc->range << '`';
  } else if (const auto* enter_info = std::get_if<DebugMarkEnterFunction>(&sc.debug_mark)) {
    os << "-> enter " << enter_info->fun_ref->as_human_readable() << ", "
       << "import " << IRVarsList(sc.var_names, enter_info->ir_import);
  } else if (const auto* leave_info = std::get_if<DebugMarkLeaveFunction>(&sc.debug_mark)) {
    os << "<- leave " << leave_info->fun_ref->as_human_readable() << ", "
       << "return " << IRVarsList(sc.var_names, leave_info->ir_return);
  } else if (const auto* local_info = std::get_if<DebugMarkLocalVar>(&sc.debug_mark)) {
    os << (local_info->local_ref->is_parameter() ? "param " : "var ") << local_info->local_ref->name
       << " " << IRVarsList(sc.var_names, local_info->ir_slots);
  } else if (const auto* sm_info = std::get_if<DebugMarkSmartCast>(&sc.debug_mark)) {
    os << sm_info->local_ref->name << " is " << sm_info->smart_cast_type->as_human_readable();
  } else if (const auto* sg_info = std::get_if<DebugMarkSetGlob>(&sc.debug_mark)) {
    os << "set_glob " << sg_info->glob_ref->name << " " << IRVarsList(sc.var_names, sg_info->ir_slots);
  } else if (std::get_if<DebugMarkScopeStart>(&sc.debug_mark)) {
    os << "scope {";
  } else if (std::get_if<DebugMarkScopeEnd>(&sc.debug_mark)) {
    os << "} scope";
  }
  return os;
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

static void output_asm_code_for_fun(std::ostream& os, FunctionPtr fun_ref, std::vector<AsmOp>&& asm_code, bool print_stack_comments, bool print_line_comments, bool emit_debug_marks) {
  tolk_assert(!asm_code.empty());
  const AsmOp& mark_enter = asm_code.front();
  tolk_assert(mark_enter.is_debug_mark() && std::holds_alternative<DebugMarkEnterFunction>(mark_enter.debug_mark));
  const std::vector<TmpVar>& var_names = std::get<FunctionBodyCode*>(fun_ref->body)->code->vars;

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
  if (print_stack_comments && !emit_debug_marks) {
    size_t len = 2 + fun_fift_name.size() + 1 + std::strlen(modifier) + 3;
    while (len < 28) {      // align "// stack state"
      os << ' ';
      len++;
    }
    os << "\t// ";
    for (var_idx_t var_idx : std::get<DebugMarkEnterFunction>(mark_enter.debug_mark).ir_import) {
      tolk_assert(var_idx < static_cast<int>(var_names.size()));
      os << ' ' << var_names[var_idx].name;
    }
  }
  os << std::endl;

  int len = static_cast<int>(asm_code.size());
  int indent = 2;
  LineCommentsOutput line_output;

  for (int i = 0; i < len; ++i) {
    AsmOp& op = asm_code[i];
    if (op.is_debug_mark() && !emit_debug_marks) {
      continue;
    }

    SrcRange range = op.origin ? op.origin->range : SrcRange::undefined();
    // origin = nullptr is for DROP, NIP, and other stack-alignment
    // (they don't have actual "origin" of execution; assigning previous/next produce spurious jumps in debugger)
    // also, `}>ELSE<{` and similar don't have origin, it's not actually an asm instruction
    bool need_loc_mark = range.is_valid();

    if (need_loc_mark && print_line_comments) {
      line_output.output_first_line(os, indent, range);
    }

    if (emit_debug_marks && need_loc_mark) {
      AsmOp op_mark = AsmOp::DebugMark(DebugMarkLocation{
        .range = range,
      });
      op_mark.a = G.debug_marks.register_debug_mark(std::move(op_mark.debug_mark));
      op_mark.output_to_fif(os, indent, print_stack_comments);
      if (print_stack_comments) {
        os << '`' << range << '`';
      }
      os << std::endl;
    }

    const DebugMarkCurrentStack* fwd_stack_op = nullptr;
    for (int j = i + 1; j < len && asm_code[j].is_debug_mark(); ++j) {
      if (const auto* stack_op = std::get_if<DebugMarkCurrentStack>(&asm_code[j].debug_mark)) {
        fwd_stack_op = stack_op;
      }
    }

    bool show_comment = print_stack_comments && (op.is_debug_mark() || (!emit_debug_marks && fwd_stack_op));
    indent -= op.op.starts_with("}>");
    if (op.is_debug_mark() && emit_debug_marks) {
      op.a = G.debug_marks.register_debug_mark(op.debug_mark);
    }
    op.output_to_fif(os, indent, show_comment);
    indent += op.op.ends_with("<{");

    if (op.is_debug_mark() && show_comment) {
      os << CommentForDebugMarkInFif(var_names, op.debug_mark);
    } else if (!emit_debug_marks && show_comment) {
      os << StackCommentInFif(fwd_stack_op->stack_slots, false);
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
    G_settings.tolk_src_as_line_comments,
    G_settings.emit_debug_marks
  );

  if (G_settings.verbosity >= 2) {
    std::cerr << "--------------\n";
  }
}

void pipeline_generate_fif_output(std::ostream& os) {
  os << "\"Asm.fif\" include\n";
  os << "// automatically generated from ";
  bool need_comma = false;
  for (SrcFilePtr file : G.all_src_files) {
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

  bool has_fun_main = false;
  bool has_onInternalMessage = false;
  int n_inlined_in_place = 0;
  std::vector<FunctionPtr> all_contract_getters;
  for (FunctionPtr fun_ref : G.all_functions) {
    if (fun_ref->is_asm_function() || !fun_ref->does_need_codegen()) {
      n_inlined_in_place += fun_ref->is_inlined_in_place() && fun_ref->is_really_used();
      continue;
    }

    if (fun_ref->is_entrypoint()) {
      has_fun_main |= fun_ref->name == "main";
      has_onInternalMessage |= fun_ref->name == "onInternalMessage";
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

  if (!has_fun_main && !has_onInternalMessage) {
    if (G_settings.allow_no_entrypoint) {
      os << "DECLPROC main // fake main\n";
    } else {
      throw Fatal("the contract has no entrypoint; forgot `fun onInternalMessage(...)`?");
    }
  }
  if (has_fun_main && has_onInternalMessage) {
    throw Fatal("both `main` and `onInternalMessage` are not allowed");
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

  if (!has_fun_main && !has_onInternalMessage) {
    os << "main PROC:<{ }>\n";  // then allow_no_entrypoint is true, checked above
  }

  for (FunctionPtr fun_ref : G.all_functions) {
    if (fun_ref->is_asm_function() || !fun_ref->does_need_codegen()) {
      continue;
    }
    generate_output_func(os, fun_ref);
  }

  if (G_settings.emit_debug_marks) {
    os << "}END>cd\n";
    // after this, on a stack: BoC (TVM bytecode) + debug marks (dict)
  } else {
    os << "}END>c\n";
    // after this, on a stack: BoC (TVM bytecode)
  }
  // TVM bytecode and marks dictionary will be ready after Fift compilation;
  // final result is handled depending on launch mode (CLI — tolk-main.cpp, or LIB/WASM — tolk-wasm.cpp)
}

} // namespace tolk
