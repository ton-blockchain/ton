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
#include "parser/symtable.h"
#include <getopt.h>
#include <fstream>
#include "git.h"

void usage(const char* progname) {
  std::cerr
      << "usage: " << progname
      << " [-vIAPSR][-O<level>][-i<indent-spc>][-o<output-filename>][-W<boc-filename>] {<func-source-filename> ...}\n"
         "\tGenerates Fift TVM assembler code from a funC source\n"
         "-I\tEnables interactive mode (parse stdin)\n"
         "-o<fift-output-filename>\tWrites generated code into specified file instead of stdout\n"
         "-v\tIncreases verbosity level (extra information output into stderr)\n"
         "-i<indent>\tSets indentation for the output code (in two-space units)\n"
         "-A\tPrefix code with `\"Asm.fif\" include` preamble\n"
         "-O<level>\tSets optimization level (2 by default)\n"
         "-P\tEnvelope code into PROGRAM{ ... }END>c\n"
         "-S\tInclude stack layout comments in the output code\n"
         "-R\tInclude operation rewrite comments in the output code\n"
         "-W<output-boc-file>\tInclude Fift code to serialize and save generated code into specified BoC file. Enables "
         "-A and -P.\n"
         "\t-s\tOutput semantic version of FunC and exit\n"
         "\t-V<version>\tShow func build information\n";
  std::exit(2);
}

int main(int argc, char* const argv[]) {
  int i;
  std::string output_filename;
  while ((i = getopt(argc, argv, "Ahi:Io:O:PRsSvW:V")) != -1) {
    switch (i) {
      case 'A':
        funC::asm_preamble = true;
        break;
      case 'I':
        funC::interactive = true;
        break;
      case 'i':
        funC::indent = std::max(0, atoi(optarg));
        break;
      case 'o':
        output_filename = optarg;
        break;
      case 'O':
        funC::opt_level = std::max(0, atoi(optarg));
        break;
      case 'P':
        funC::program_envelope = true;
        break;
      case 'R':
        funC::op_rewrite_comments = true;
        break;
      case 'S':
        funC::stack_layout_comments = true;
        break;
      case 'v':
        ++funC::verbosity;
        break;
      case 'W':
        funC::boc_output_filename = optarg;
        funC::asm_preamble = funC::program_envelope = true;
        break;
      case 's':
        std::cout << funC::func_version << "\n";
        std::exit(0);
        break;
      case 'V':
        std::cout << "FunC semantic version: v" << funC::func_version << "\n";
        std::cout << "Build information: [ Commit: " << GitMetadata::CommitSHA1() << ", Date: " << GitMetadata::CommitDate() << "]\n";
        std::exit(0);
        break;
      case 'h':
      default:
        usage(argv[0]);
    }
  }

  std::ostream *outs = &std::cout;

  std::unique_ptr<std::fstream> fs;
  if (!output_filename.empty()) {
    fs = std::make_unique<std::fstream>(output_filename, fs->trunc | fs->out);
    if (!fs->is_open()) {
      std::cerr << "failed to create output file " << output_filename << '\n';
      return 2;
    }
    outs = fs.get();
  }

  std::vector<std::string> sources;

  while (optind < argc) {
    sources.push_back(std::string(argv[optind++]));
  }

  return funC::func_proceed(sources, *outs, std::cerr);
}
