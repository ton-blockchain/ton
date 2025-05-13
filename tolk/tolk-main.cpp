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
#include <sys/stat.h>
#ifdef TD_DARWIN
#include <mach-o/dyld.h>
#elif TD_WINDOWS
#include <windows.h>
#else  // linux
#include <unistd.h>
#endif
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
         "-L\tDon't include original lines from Tolk src into Fift output\n"
         "-e\tIncreases verbosity level (extra output into stderr)\n"
         "-v\tOutput version of Tolk and exit\n";
  std::exit(2);
}

static bool stdlib_folder_exists(const char* stdlib_folder) {
  struct stat f_stat;
  int res = stat(stdlib_folder, &f_stat);
  return res == 0 && (f_stat.st_mode & S_IFMT) == S_IFDIR;
}

// getting current executable path is a complicated and not cross-platform task
// for instance, we can't just use argv[0] or even filesystem::canonical
// https://stackoverflow.com/questions/1023306/finding-current-executables-path-without-proc-self-exe/1024937
static bool get_current_executable_filename(std::string& out) {
#ifdef TD_DARWIN
  char name_buf[1024];
  unsigned int size = 1024;
  if (0 == _NSGetExecutablePath(name_buf, &size)) {   // may contain ../, so normalize it
    char *exe_path = realpath(name_buf, nullptr);
    if (exe_path != nullptr) {
      out = exe_path;
      return true;
    }
  }
#elif TD_WINDOWS
  char exe_path[1024];
  if (GetModuleFileNameA(nullptr, exe_path, 1024)) {
    out = exe_path;
    std::replace(out.begin(), out.end(), '\\', '/');    // modern Windows correctly deals with / separator
    return true;
  }
#else  // linux
  char exe_path[1024];
  ssize_t res = readlink("/proc/self/exe", exe_path, 1024 - 1);
  if (res >= 0) {
    exe_path[res] = 0;
    out = exe_path;
    return true;
  }
#endif
  return false;
}

// simple join "/some/folder/" (guaranteed to end with /) and "../relative/path"
static std::string join_path(std::string dir, const char* relative) {
  while (relative[0] == '.' && relative[1] == '.' && relative[2] == '/') {
    size_t slash_pos = dir.find_last_of('/', dir.size() - 2);   // last symbol is slash, find before it
    if (slash_pos != std::string::npos) {
      dir = dir.substr(0, slash_pos + 1);
    }
    relative += 3;
  }

  return dir + relative;
}

static std::string auto_discover_stdlib_folder() {
  // if the user launches tolk compiler from a package installed (e.g. /usr/bin/tolk),
  // locate stdlib in /usr/share/ton/smartcont (this folder exists on package installation)
  // (note, that paths are not absolute, they are relative to the launched binary)
  // consider https://github.com/ton-blockchain/packages for actual paths
  std::string executable_filename;
  if (!get_current_executable_filename(executable_filename)) {
    return {};
  }

  // extract dirname to concatenate with relative paths (separator / is ok even for windows)
  size_t slash_pos = executable_filename.find_last_of('/');
  std::string executable_dir = executable_filename.substr(0, slash_pos + 1);

#ifdef TD_DARWIN
  std::string def_location = join_path(executable_dir, "../share/ton/ton/smartcont/tolk-stdlib");
#elif TD_WINDOWS
  std::string def_location = join_path(executable_dir, "smartcont/tolk-stdlib");
#else  // linux
  std::string def_location = join_path(executable_dir, "../share/ton/smartcont/tolk-stdlib");
#endif

  if (stdlib_folder_exists(def_location.c_str())) {
    return def_location;
  }

  // so, the binary is not from a system package
  // maybe it's just built from sources? e.g. ~/ton/cmake-build-debug/tolk/tolk
  // then, check the ~/ton/crypto/smartcont folder
  std::string near_when_built_from_sources = join_path(executable_dir, "../../crypto/smartcont/tolk-stdlib");
  if (stdlib_folder_exists(near_when_built_from_sources.c_str())) {
    return near_when_built_from_sources;
  }

  // no idea of where to find stdlib; let's show an error for the user, he should provide env var above
  return {};
}

td::Result<std::string> fs_read_callback(CompilerSettings::FsReadCallbackKind kind, const char* query) {
  switch (kind) {
    case CompilerSettings::FsReadCallbackKind::Realpath: {
      td::Result<std::string> res_realpath;
      if (query[0] == '@' && strlen(query) > 8 && !strncmp(query, "@stdlib/", 8)) {
        // import "@stdlib/filename" or import "@stdlib/filename.tolk"
        std::string path = G.settings.stdlib_folder + static_cast<std::string>(query + 7);
        if (strncmp(path.c_str() + path.size() - 5, ".tolk", 5) != 0) {
          path += ".tolk";
        }
        res_realpath = td::realpath(td::CSlice(path.c_str()));
      } else {
        // import "relative/to/cwd/path.tolk"
        res_realpath = td::realpath(td::CSlice(query));
      }

      if (res_realpath.is_error()) {
        // note, that for non-existing files, `realpath()` on Linux/Mac returns an error,
        // whereas on Windows, it returns okay, but fails after, on reading, with a message "cannot open file"
        return td::Status::Error(std::string{"cannot find file "} + query);
      }
      return res_realpath;
    }
    case CompilerSettings::FsReadCallbackKind::ReadFile: {
      struct stat f_stat;
      int res = stat(query, &f_stat);   // query here is already resolved realpath
      if (res != 0 || (f_stat.st_mode & S_IFMT) != S_IFREG) {
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
    default: {
      return td::Status::Error("unknown query kind");
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
  while ((i = getopt(argc, argv, "o:b:O:x:SLevh")) != -1) {
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
      case 'L':
        G.settings.tolk_src_as_line_comments = false;
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

  // locate tolk-stdlib/ based on env or default system paths
  if (const char* env_var = getenv("TOLK_STDLIB")) {
    std::string stdlib_filename = static_cast<std::string>(env_var) + "/common.tolk";
    td::Result<std::string> res = td::realpath(td::CSlice(stdlib_filename.c_str()));
    if (res.is_error()) {
      std::cerr << "Environment variable TOLK_STDLIB is invalid: " << res.move_as_error().message().c_str() << std::endl;
      return 2;
    }
    G.settings.stdlib_folder = env_var;
  } else {
    G.settings.stdlib_folder = auto_discover_stdlib_folder();
  }
  if (G.settings.stdlib_folder.empty()) {
    std::cerr << "Failed to discover Tolk stdlib.\n"
                 "Probably, you have a non-standard Tolk installation.\n"
                 "Please, provide env variable TOLK_STDLIB referencing to tolk-stdlib/ folder.\n";
    return 2;
  }
  if (G.is_verbosity(2)) {
    std::cerr << "stdlib folder: " << G.settings.stdlib_folder << std::endl;
  }

  if (optind != argc - 1) {
    std::cerr << "invalid usage: should specify exactly one input file.tolk" << std::endl;
    return 2;
  }

  G.settings.read_callback = fs_read_callback;

  int exit_code = tolk_proceed(argv[optind]);
  return exit_code;
}
