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
#include <getopt.h>
#include <fstream>
#include "git.h"

void usage(const char* progname) {
  std::cerr
      << "usage: " << progname << " [options] <filename.tolk>\n"
         "\tGenerates Fift TVM assembler code from a .tolk file\n"
         "-o<fif-filename>\tWrites generated code into specified .fif file instead of stdout\n"
         "-b<boc-filename>\tGenerate Fift instructions to save TVM bytecode into .boc file\n"
         "-O<level>\tSets optimization level (2 by default)\n"
         "-S\tDon't include stack layout comments into Fift output\n"
         "-e\tIncreases verbosity level (extra output into stderr)\n"
         "-v\tOutput version of Tolk and exit\n";
  std::exit(2);
}

int main(int argc, char* const argv[]) {
  int i;
  std::string output_filename;
  while ((i = getopt(argc, argv, "o:b:O:Sevh")) != -1) {
    switch (i) {
      case 'o':
        output_filename = optarg;
        break;
      case 'b':
        tolk::boc_output_filename = optarg;
        break;
      case 'O':
        tolk::opt_level = std::max(0, atoi(optarg));
        break;
      case 'S':
        tolk::stack_layout_comments = false;
        break;
      case 'e':
        ++tolk::verbosity;
        break;
      case 'v':
        std::cout << "Tolk compiler v" << tolk::tolk_version << "\n";
        std::cout << "Build commit: " << GitMetadata::CommitSHA1() << "\n";
        std::cout << "Build date: " << GitMetadata::CommitDate() << "\n";
        std::exit(0);
      case 'h':
      default:
        usage(argv[0]);
    }
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

  if (optind != argc - 1) {
    std::cerr << "invalid usage: should specify exactly one input file.tolk";
    return 2;
  }

  std::string entrypoint_file_name = argv[optind];

  tolk::read_callback = tolk::fs_read_callback;

  return tolk::tolk_proceed(entrypoint_file_name, *outs, std::cerr);
}
