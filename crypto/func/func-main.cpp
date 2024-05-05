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
#include "td/utils/OptionParser.h"
#include <fstream>
#include "git.h"

int main(int argc, char* argv[]) {
  std::string output_filename;
  td::OptionParser p;
  p.set_description("usage: func "
      "[-vIAPSR][-O<level>][-i<indent-spc>][-o<output-filename>][-W<boc-filename>] {<func-source-filename> ...}\n"
      "Generates Fift TVM assembler code from FunC sources");

  p.add_option('I', "interactive", "Enables interactive mode (parse stdin)", [] {
    funC::interactive = true;
  });
  p.add_option('o', "output", "Writes generated code into specified file instead of stdout", [&output_filename](td::Slice arg) {
    output_filename = arg.str();
  });
  p.add_option('v', "verbose", "Increases verbosity level (extra information output into stderr)", [] {
    ++funC::verbosity;
  });
  p.add_option('i', "indent", "Sets indentation for the output code (in two-space units)", [](td::Slice arg) {
    funC::indent = std::max(0, std::atoi(arg.str().c_str()));
  });
  p.add_option('A', "asm-preamble", "prefix code with `\"Asm.fif\" include` preamble", [] {
    funC::asm_preamble = true;
  });
  p.add_option('O', "opt-level", "Sets optimization level (2 by default)", [](td::Slice arg) {
    funC::opt_level = std::max(0, std::atoi(arg.str().c_str()));
  });
  p.add_option('P', "program-envelope", "Envelope code into PROGRAM{ ... }END>c", [] {
    funC::program_envelope = true;
  });
  p.add_option('S', "stack-comments", "Include stack layout comments in the output code", []{
    funC::stack_layout_comments = true;
  });
  p.add_option('R', "rewrite-comments", "Include operation rewrite comments in the output code", [] {
    funC::op_rewrite_comments = true;
  });
  p.add_option('W', "boc-output", "Include Fift code to serialize and save generated code into specified BoC file. Enables -A and -P", [](td::Slice arg) {
    funC::boc_output_filename = arg.str();
    funC::asm_preamble = funC::program_envelope = true;
  });
  p.add_option('s', "version", "Output semantic version of FunC and exit", [] {
    std::cout << funC::func_version << "\n";
    std::exit(0);
  });
  p.add_option('V', "full-version", "Show FunC build information and exit", [] {
    std::cout << "FunC semantic version: v" << funC::func_version << "\n";
    std::cout << "Build information: [ Commit: " << GitMetadata::CommitSHA1() << ", Date: " << GitMetadata::CommitDate() << "]\n";
    std::exit(0);
  });
  p.add_option('h', "help", "Print help and exit", [&p] {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });

  auto parse_result = p.run(argc, argv);
  if (parse_result.is_error()) {
    std::cerr << "failed to parse options: " << parse_result.error().message().c_str() << "\n";
    return 2;
  }

  std::ostream *outs = &std::cout;

  std::unique_ptr<std::fstream> fs;
  if (!output_filename.empty()) {
    fs = std::make_unique<std::fstream>(output_filename, std::fstream::trunc | std::fstream::out);
    if (!fs->is_open()) {
      std::cerr << "failed to create output file " << output_filename << '\n';
      return 2;
    }
    outs = fs.get();
  }

  std::vector<std::string> sources;

  for (const char* in_filename : parse_result.ok()) {
    sources.emplace_back(in_filename);
  }

  funC::read_callback = funC::fs_read_callback;

  return funC::func_proceed(sources, *outs, std::cerr);
}
