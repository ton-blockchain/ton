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

namespace funC {

int verbosity, indent, opt_level = 2;
bool stack_layout_comments, op_rewrite_comments, program_envelope, asm_preamble;
bool interactive = false;
std::string generated_from, boc_output_filename;

/*
 *
 *   OUTPUT CODE GENERATOR
 *
 */

void generate_output_func(SymDef* func_sym, std::ostream &outs, std::ostream &errs) {
  SymValCodeFunc* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
  assert(func_val);
  std::string name = sym::symbols.get_name(func_sym->sym_idx);
  if (verbosity >= 2) {
    errs << "\n\n=========================\nfunction " << name << " : " << func_val->get_type() << std::endl;
  }
  if (!func_val->code) {
    errs << "( function `" << name << "` undefined )\n";
    throw src::ParseError(func_sym->loc, name);
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
    bool inline_func = (func_val->flags & 1);
    bool inline_ref = (func_val->flags & 2);
    const char* modifier = "";
    if (inline_func) {
      modifier = "INLINE";
    } else if (inline_ref) {
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
    auto fv = dynamic_cast<const SymValCodeFunc*>(func_sym->value);
    // Flags: 1 - inline, 2 - inline_ref
    if (fv && (fv->flags & 1) && code.ops->noreturn()) {
      mode |= Stack::_InlineFunc;
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
  for (SymDef* func_sym : glob_func) {
    SymValCodeFunc* func_val = dynamic_cast<SymValCodeFunc*>(func_sym->value);
    assert(func_val);
    std::string name = sym::symbols.get_name(func_sym->sym_idx);
    outs << std::string(indent * 2, ' ');
    if (func_val->method_id.is_null()) {
      outs << "DECLPROC " << name << "\n";
    } else {
      outs << func_val->method_id << " DECLMETHOD " << name << "\n";
    }
  }
  for (SymDef* gvar_sym : glob_vars) {
    assert(dynamic_cast<SymValGlobVar*>(gvar_sym->value));
    std::string name = sym::symbols.get_name(gvar_sym->sym_idx);
    outs << std::string(indent * 2, ' ') << "DECLGLOBVAR " << name << "\n";
  }
  int errors = 0;
  for (SymDef* func_sym : glob_func) {
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
      ok += funC::parse_source_file(src.c_str());
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