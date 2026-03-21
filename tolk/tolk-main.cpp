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
#include "tolk-version.h"
#include "compiler-state.h"
#include "compiler-settings.h"
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
#include "json-output.h"

using namespace tolk;

enum LongOnlyOptions {
  OPT_BOC_OUTPUT = 256,
  OPT_PATH_MAPPING,
  OPT_NO_STACK_COMMENTS,
  OPT_NO_LINE_COMMENTS,
  OPT_JSON_ERRORS,
  OPT_CHECK_ONLY,
  OPT_ALLOW_NO_ENTRYPOINT,
};

static struct option long_options[] = {
  {"output", required_argument, nullptr, 'o'},
  {"boc-output", required_argument, nullptr, OPT_BOC_OUTPUT},
  {"opt-level", required_argument, nullptr, 'O'},
  {"path-mapping", required_argument, nullptr, OPT_PATH_MAPPING},
  {"no-stack-comments", no_argument, nullptr, OPT_NO_STACK_COMMENTS},
  {"no-line-comments", no_argument, nullptr, OPT_NO_LINE_COMMENTS},
  {"json-errors", no_argument, nullptr, OPT_JSON_ERRORS},
  {"check-only", no_argument, nullptr, OPT_CHECK_ONLY},
  {"allow-no-entrypoint", no_argument, nullptr, OPT_ALLOW_NO_ENTRYPOINT},
  {"verbose", no_argument, nullptr, 'e'},
  {"version", no_argument, nullptr, 'V'},
  {"help", no_argument, nullptr, 'h'},
  {nullptr, 0, nullptr, 0}
};

void usage(const char* progname) {
  std::cerr
      << "usage: " << progname << " [options] <filename.tolk>\n"
            "\tGenerates Fift TVM assembler code from a .tolk file\n"
         "-o, --output <fif-filename>\n"
            "\tWrite generated code into specified .fif file instead of stdout\n"
         "--boc-output <boc-filename>\n"
            "\tGenerate Fift instructions to save TVM bytecode into .boc file\n"
         "-O, --opt-level <level>\n"
            "\tSet optimization level (2 by default)\n"
         "--path-mapping <mapping>\n"
            "\tRegister @name -> path mapping (e.g. @mylib=/path/to/lib)\n"
         "--no-stack-comments\n"
            "\tDon't include stack layout comments into Fift output\n"
         "--no-line-comments\n"
            "\tDon't include original lines from Tolk src into Fift output\n"
         "--json-errors\n"
            "\tShow compilation errors in JSON (not human-readable) format\n"
         "--check-only\n"
            "\tCheck sources for errors without generating code (for IDE in background)\n"
         "--allow-no-entrypoint\n"
            "\tDo not require main/onInternalMessage (e.g. to compile only get-methods)\n"
         "-e, --verbose\n"
            "\tIncrease verbosity level (extra output into stderr)\n"
         "-v, --version\n"
            "\tOutput version of Tolk and exit\n"
         "-h, --help\n"
            "\tShow this help message\n";
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

td::Result<std::string> fs_read_callback(CompilerSettings::FsReadCallbackKind kind, const char* query, void* callback_payload) {
  switch (kind) {
    case CompilerSettings::FsReadCallbackKind::Realpath: {
      std::string path;
      if (query[0] == '@' && strlen(query) > 8 && !strncmp(query, "@stdlib/", 8)) {
        path = G_settings.stdlib_folder + static_cast<std::string>(query + 7);
      } else if (query[0] == '@') {
        const char* slash = strchr(query, '/');
        if (slash == nullptr || slash[1] == '\0') {
          return td::Status::Error("import path with @ prefix must specify a file, e.g. @third_party/math-utils");
        }
        std::string_view at_prefix(query, slash);
        std::string_view abs_folder = G_settings.get_path_mapping(at_prefix);
        if (abs_folder.empty()) {
          return td::Status::Error("path mapping " + std::string{at_prefix} + " was not registered");
        }
        path = std::string(abs_folder) + slash;
      } else {
        path = query;
      }

      // reject `import "some/dir/"`, do not try to load "some/dir/.tolk"
      if (path.back() == '/' || path.back() == '\\') {
        return td::Status::Error("import path must specify a file, not a directory");
      }

      if (strncmp(path.c_str() + path.size() - 5, ".tolk", 5) != 0) {
        path += ".tolk";
      }
      td::Result<std::string> res_realpath = td::realpath(td::CSlice(path.c_str()));
      if (res_realpath.is_error()) {
        // note, that for non-existing files, `realpath()` on Linux/Mac returns an error,
        // whereas on Windows, it returns okay, but fails after, on reading, with a message "cannot open file"
        return td::Status::Error("cannot find file \"" + path + "\"");
      }
      // files with the same realpath are considered equal (imported only once)
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
      if (!f) {
        return td::Status::Error(std::string{"cannot open file "} + query);
      }
      fread(str.data(), file_size, 1, f);
      fclose(f);
      return std::move(str);
    }
    default: {
      return td::Status::Error("unknown query kind");
    }
  }

  // callback_payload is not used in CLI mode, it's for library mode, see tolk-wasm.cpp
  static_cast<void>(callback_payload);
}

GNU_ATTRIBUTE_NOINLINE
static void compilation_failed_output_errors(const std::vector<ThrownParseError>& errors) {
  constexpr int JSON_ERROR_LIMIT = 50;
  constexpr int CONSOLE_ERROR_LIMIT = 20;
  int shown = 0;

  if (G_settings.show_errors_as_json) {
    JsonPrettyOutput json(std::cerr);
    json.start_object();
    json.key_value("status", "error");
    json.start_array("errors");
    for (const ThrownParseError& error : errors) {
      if (shown >= JSON_ERROR_LIMIT) break;
      error.output_to_json(json);
      shown++;
    }
    json.end_array();
    json.end_object();

  } else {
    for (const ThrownParseError& error : errors) {
      if (shown >= CONSOLE_ERROR_LIMIT) break;
      if (shown++) std::cerr << std::endl;  // separator between errors
      error.output_to_console(std::cerr);
    }
  }
}

static void compilation_failed_with_fatal(const std::string& message) {
  // no location, no pretty header, no json output, just "fatal", something unexpected happened
  std::cerr << "fatal: " << message << std::endl;
}

static void compilation_succeed_after_output_done() {
  if (G_settings.show_errors_as_json) {
    std::cerr << R"({"status":"ok"})";
  }
}

int main(int argc, char* const argv[]) {
  int i;
  while ((i = getopt_long(argc, argv, "o:O:evVh", long_options, nullptr)) != -1) {
    switch (i) {
      case 'o':
        G_settings.output_filename = optarg;
        break;
      case OPT_BOC_OUTPUT:
        G_settings.boc_output_filename = optarg;
        break;
      case 'O':
        G_settings.optimization_level = std::max(0, atoi(optarg));
        break;
      case OPT_PATH_MAPPING:
        if (!G_settings.parse_path_mapping_cmd_arg(optarg)) {
          return 2;   // the error was printed to std::cerr
        }
        break;
      case OPT_NO_STACK_COMMENTS:
        G_settings.stack_layout_comments = false;
        break;
      case OPT_NO_LINE_COMMENTS:
        G_settings.tolk_src_as_line_comments = false;
        break;
      case OPT_JSON_ERRORS:
        G_settings.show_errors_as_json = true;
        break;
      case OPT_CHECK_ONLY:
        G_settings.check_only_no_output = true;
        break;
      case OPT_ALLOW_NO_ENTRYPOINT:
        G_settings.allow_no_entrypoint = true;
        break;
      case 'e':
        G_settings.verbosity++;
        break;
      case 'v':
      case 'V':
        std::cout << "Tolk compiler v" << TOLK_VERSION << std::endl;
        std::cout << "Build commit: " << GitMetadata::CommitSHA1() << std::endl;
        std::cout << "Build date: " << GitMetadata::CommitDate() << std::endl;
        std::exit(0);
      case 'h':
      default:
        usage(argv[0]);
    }
  }

  // locate tolk-stdlib/ based on env or default system paths
  if (const char* env_var = getenv("TOLK_STDLIB")) {
    std::string stdlib_filename = static_cast<std::string>(env_var) + "/common.tolk";
    td::Result<std::string> res = td::realpath(td::CSlice(stdlib_filename.c_str()));
    if (res.is_error()) {
      std::cerr << "Environment variable TOLK_STDLIB is invalid: " << res.move_as_error().message().c_str() << std::endl;
      return 2;
    }
    G_settings.stdlib_folder = env_var;
  } else {
    G_settings.stdlib_folder = auto_discover_stdlib_folder();
  }
  if (G_settings.stdlib_folder.empty()) {
    std::cerr << "Failed to discover Tolk stdlib.\n"
                 "Probably, you have a non-standard Tolk installation.\n"
                 "Please, provide env variable TOLK_STDLIB referencing to tolk-stdlib/ folder.\n";
    return 2;
  }
  if (G_settings.verbosity >= 2) {
    std::cerr << "stdlib folder: " << G_settings.stdlib_folder << std::endl;
  }

  if (optind != argc - 1) {
    std::cerr << "invalid usage: should specify exactly one input file.tolk" << std::endl;
    return 2;
  }

  G_settings.read_callback = fs_read_callback;

  TolkCompilationResult result = tolk_proceed(argv[optind]);
  if (!result.fatal_msg.empty()) {
    compilation_failed_with_fatal(result.fatal_msg);
    return 2;
  }
  if (!result.errors.empty()) {
    compilation_failed_output_errors(result.errors);
    return 2;
  }

  // for IDE in background: no codegen, do not create or truncate output files
  if (G_settings.check_only_no_output) {
    compilation_succeed_after_output_done();
    return 0;
  }

  // if output filename is empty, no files are written (only Fift code is written into stdout)
  if (G_settings.output_filename.empty()) {
    std::cout << result.fift_code;
    compilation_succeed_after_output_done();
    return 0;
  }

  std::ofstream fif_out_file(G_settings.output_filename);
  if (!fif_out_file.is_open()) {
    std::cerr << "Failed to create output file " << G_settings.output_filename << std::endl;
    return 2;
  }
  fif_out_file << result.fift_code;

  compilation_succeed_after_output_done();
  return 0;
}
