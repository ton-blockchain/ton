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
#include "tolk-version.h"
#include "compiler-state.h"
#include "td/utils/port/path.h"
#include <getopt.h>
#include <fstream>
#include <utility>
#include <sys/stat.h>
#include <filesystem>
#include "git.h"

using namespace tolk;

void usage(const char* progname) {
  std::cerr
      << "usage: " << progname << " [options] <filename.tolk>\n"
         "\tGenerates Fift TVM assembler code from a .tolk file\n"
         "-o<fif-filename>\tWrites generated code into specified .fif file instead of stdout\n"
         "-b<boc-filename>\tGenerate Fift instructions to save TVM bytecode into .boc file\n"
         "-O<level>\tSets optimization level (2 by default)\n"
         "-x<option-names>\tEnables experimental options, comma-separated\n"
         "-S\tDon't include stack layout comments into Fift output\n"
         "-e\tIncreases verbosity level (extra output into stderr)\n"
         "-v\tOutput version of Tolk and exit\n";
  std::exit(2);
}

static bool stdlib_file_exists(std::filesystem::path& stdlib_tolk) {
  struct stat f_stat;
  stdlib_tolk = stdlib_tolk.lexically_normal();
  int res = stat(stdlib_tolk.c_str(), &f_stat);
  return res == 0 && S_ISREG(f_stat.st_mode);
}

static std::string auto_discover_stdlib_location(const char* argv0) {
  // first, the user can specify env var that points directly to stdlib (useful for non-standard compiler locations)
  if (const char* env_var = getenv("TOLK_STDLIB")) {
    return env_var;
  }

  // if the user launches tolk compiler from a package installed (e.g. /usr/bin/tolk),
  // locate stdlib in /usr/share/ton/smartcont (this folder exists on package installation)
  // (note, that paths are not absolute, they are relative to the launched binary)
  // consider https://github.com/ton-blockchain/packages for actual paths
  std::filesystem::path executable_dir = std::filesystem::canonical(argv0).remove_filename();

#ifdef TD_DARWIN
  auto def_location = executable_dir / "../share/ton/ton/smartcont/stdlib.tolk";
#elif TD_WINDOWS
  auto def_location = executable_dir / "smartcont/stdlib.tolk";
#else  // linux
  auto def_location = executable_dir / "../share/ton/smartcont/stdlib.tolk";
#endif

  if (stdlib_file_exists(def_location)) {
    return def_location;
  }

  // so, the binary is not from a system package
  // maybe it's just built from sources? e.g. ~/ton/cmake-build-debug/tolk/tolk
  // then, check the ~/ton/crypto/smartcont folder
  auto near_when_built_from_sources = executable_dir / "../../crypto/smartcont/stdlib.tolk";
  if (stdlib_file_exists(near_when_built_from_sources)) {
    return near_when_built_from_sources;
  }

  // no idea of where to find stdlib; let's show an error for the user, he should provide env var above
  return {};
}

td::Result<std::string> fs_read_callback(CompilerSettings::FsReadCallbackKind kind, const char* query) {
  switch (kind) {
    case CompilerSettings::FsReadCallbackKind::ReadFile: {
      struct stat f_stat;
      int res = stat(query, &f_stat);
      if (res != 0 || !S_ISREG(f_stat.st_mode)) {
        return td::Status::Error(std::string{"cannot open file "} + query);
      }

      size_t file_size = static_cast<size_t>(f_stat.st_size);
      std::string str;
      str.resize(file_size);
      FILE* f = fopen(query, "rb");
      fread(str.data(), file_size, 1, f);
      fclose(f);
      return std::move(str);
    }
    case CompilerSettings::FsReadCallbackKind::Realpath: {
      td::Result<std::string> res_realpath = td::realpath(td::CSlice(query));
      if (res_realpath.is_error()) {
        return td::Status::Error(std::string{"cannot find file "} + query);
      }
      return res_realpath;
    }
    default: {
      return td::Status::Error("Unknown query kind");
    }
  }
}

class StdCoutRedirectToFile {
  std::unique_ptr<std::fstream> output_file;
  std::streambuf* backup_sbuf = nullptr;

public:
  explicit StdCoutRedirectToFile(const std::string& output_filename) {
    if (!output_filename.empty()) {
      output_file = std::make_unique<std::fstream>(output_filename, std::fstream::trunc | std::fstream::out);
      if (output_file->is_open()) {
        backup_sbuf = std::cout.rdbuf(output_file->rdbuf());
      }
    }
  }

  ~StdCoutRedirectToFile() {
    if (backup_sbuf) {
      std::cout.rdbuf(backup_sbuf);
    }
  }

  bool is_failed() const { return output_file && !output_file->is_open(); }
};

int main(int argc, char* const argv[]) {
  int i;
  while ((i = getopt(argc, argv, "o:b:O:x:Sevh")) != -1) {
    switch (i) {
      case 'o':
        G.settings.output_filename = optarg;
        break;
      case 'b':
        G.settings.boc_output_filename = optarg;
        break;
      case 'O':
        G.settings.optimization_level = std::max(0, atoi(optarg));
        break;
      case 'x':
        G.settings.parse_experimental_options_cmd_arg(optarg);
        break;
      case 'S':
        G.settings.stack_layout_comments = false;
        break;
      case 'e':
        G.settings.verbosity++;
        break;
      case 'v':
        std::cout << "Tolk compiler v" << TOLK_VERSION << std::endl;
        std::cout << "Build commit: " << GitMetadata::CommitSHA1() << std::endl;
        std::cout << "Build date: " << GitMetadata::CommitDate() << std::endl;
        std::exit(0);
      case 'h':
      default:
        usage(argv[0]);
    }
  }

  StdCoutRedirectToFile redirect_cout(G.settings.output_filename);
  if (redirect_cout.is_failed()) {
    std::cerr << "Failed to create output file " << G.settings.output_filename << std::endl;
    return 2;
  }

  // locate stdlib.tolk based on env or default system paths
  G.settings.stdlib_filename = auto_discover_stdlib_location(argv[0]);
  if (G.settings.stdlib_filename.empty()) {
    std::cerr << "Failed to discover stdlib.tolk.\n"
                 "Probably, you have a non-standard Tolk installation.\n"
                 "Please, provide env variable TOLK_STDLIB referencing to it.\n";
    return 2;
  }
  if (G.is_verbosity(2)) {
    std::cerr << "stdlib located at " << G.settings.stdlib_filename << std::endl;
  }

  if (optind != argc - 1) {
    std::cerr << "invalid usage: should specify exactly one input file.tolk" << std::endl;
    return 2;
  }

  G.settings.entrypoint_filename = argv[optind];
  G.settings.read_callback = fs_read_callback;

  return tolk_proceed(G.settings.entrypoint_filename);
}
