/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "utils.h"
#include "words.h"
#include "td/utils/PathView.h"
#include "td/utils/filesystem.h"
#include "td/utils/misc.h"
#include "td/utils/port/path.h"
#include "vm/boc.h"
#include <map>

namespace fift {
namespace {

std::string fift_dir(std::string dir) {
  return dir.size() > 0 ? dir : td::PathView(td::realpath(__FILE__).move_as_ok()).parent_dir().str() + "lib/";
}
std::string smartcont_dir(std::string dir) {
  return dir.size() > 0
             ? dir
             : td::PathView(td::PathView(td::realpath(__FILE__).move_as_ok()).parent_dir_noslash()).parent_dir().str() +
                   "smartcont/";
}
td::Result<std::string> load_source(std::string name, std::string dir = "") {
  return td::read_file_str(fift_dir(dir) + name);
}
td::Result<std::string> load_Fift_fif(std::string dir = "") {
  return load_source("Fift.fif", dir);
}
td::Result<std::string> load_Asm_fif(std::string dir = "") {
  return load_source("Asm.fif", dir);
}
td::Result<std::string> load_TonUtil_fif(std::string dir = "") {
  return load_source("TonUtil.fif", dir);
}
td::Result<std::string> load_Lists_fif(std::string dir = "") {
  return load_source("Lists.fif", dir);
}
td::Result<std::string> load_Lisp_fif(std::string dir = "") {
  return load_source("Lisp.fif", dir);
}
td::Result<std::string> load_GetOpt_fif(std::string dir = "") {
  return load_source("GetOpt.fif", dir);
}
td::Result<std::string> load_wallet3_code_fif(std::string dir = "") {
  return td::read_file_str(smartcont_dir(dir) + "wallet-v3-code.fif");
}
td::Result<std::string> load_FiftExt_fif(std::string dir = "") {
  return load_source("FiftExt.fif", dir);
}
td::Result<std::string> load_Disasm_fif(std::string dir = "") {
  return load_source("Disasm.fif", dir);
}

class MemoryFileLoader : public fift::FileLoader {
 public:
  td::Result<fift::FileLoader::File> read_file(td::CSlice filename) override {
    auto it = files_.find(filename);
    if (it == files_.end()) {
      return td::Status::Error("File not found");
    }
    fift::FileLoader::File res;
    res.data = it->second;
    res.path = it->first;
    return std::move(res);
  }

  td::Status write_file(td::CSlice filename, td::Slice data) override {
    files_[filename.str()] = data.str();
    return td::Status::OK();
  }

  void add_file(std::string path, std::string data) {
    files_[path] = std::move(data);
  }
  td::Result<File> read_file_part(td::CSlice filename, td::int64 size, td::int64 offset) override {
    auto it = files_.find(filename);
    if (it == files_.end()) {
      return td::Status::Error("File not found");
    }
    fift::FileLoader::File res;
    if (static_cast<td::int64>(it->second.size()) < offset) {
      return td::Status::Error("Offset too large");
    }
    if (size > static_cast<td::int64>(it->second.size())) {
      size = static_cast<td::int64>(it->second.size());
    }
    res.data = td::Slice(it->second).substr(td::narrow_cast<size_t>(offset), td::narrow_cast<size_t>(size)).str();
    res.path = it->first;
    return std::move(res);
  }

  bool is_file_exists(td::CSlice filename) override {
    return files_.count(filename) != 0;
  }

 private:
  std::map<std::string, std::string, std::less<>> files_;
};

td::Result<fift::SourceLookup> create_source_lookup(std::string&& main, bool need_preamble = true, bool need_asm = true,
                                                    bool need_ton_util = true, bool need_lisp = true,
                                                    bool need_w3_code = true, bool need_fift_ext = true,
                                                    bool need_disasm = true, std::string dir = "") {
  auto loader = std::make_unique<MemoryFileLoader>();
  loader->add_file("/main.fif", std::move(main));
  if (need_preamble) {
    TRY_RESULT(f, load_Fift_fif(dir));
    loader->add_file("/Fift.fif", std::move(f));
  }
  if (need_asm) {
    TRY_RESULT(f, load_Asm_fif(dir));
    loader->add_file("/Asm.fif", std::move(f));
  }
  if (need_ton_util) {
    {
      TRY_RESULT(f, load_Lists_fif(dir));
      loader->add_file("/Lists.fif", std::move(f));
    }
    {
      TRY_RESULT(f, load_TonUtil_fif(dir));
      loader->add_file("/TonUtil.fif", std::move(f));
    }
    {
      TRY_RESULT(f, load_GetOpt_fif(dir));
      loader->add_file("/GetOpt.fif", std::move(f));
    }
  }
  if (need_lisp) {
    TRY_RESULT(f, load_Lisp_fif(dir));
    loader->add_file("/Lisp.fif", std::move(f));
  }
  if (need_w3_code) {
    TRY_RESULT(f, load_wallet3_code_fif(dir));
    loader->add_file("/wallet-v3-code.fif", std::move(f));
  }
  if (need_fift_ext) {
    TRY_RESULT(f, load_FiftExt_fif(dir));
    loader->add_file("/FiftExt.fif", std::move(f));
  }
  if (need_disasm) {
    TRY_RESULT(f, load_Disasm_fif(dir));
    loader->add_file("/Disasm.fif", std::move(f));
  }
  auto res = fift::SourceLookup(std::move(loader));
  res.add_include_path("/");
  return std::move(res);
}

td::Result<fift::SourceLookup> run_fift(fift::SourceLookup source_lookup, std::ostream *stream,
                                        bool preload_fift = true, std::vector<std::string> args = {}) {
  fift::Fift::Config config;
  config.source_lookup = std::move(source_lookup);
  fift::init_words_common(config.dictionary);
  fift::init_words_vm(config.dictionary);
  fift::init_words_ton(config.dictionary);
  config.error_stream = stream;
  config.output_stream = stream;
  if (args.size() != 0) {
    std::vector<const char *> argv;
    for (auto &arg : args) {
      argv.push_back(arg.c_str());
    }
    fift::import_cmdline_args(config.dictionary, argv[0], td::narrow_cast<int>(argv.size() - 1), argv.data() + 1);
  }
  fift::Fift fift{std::move(config)};
  if (preload_fift) {
    TRY_STATUS(fift.interpret_file("Fift.fif", ""));
  }
  TRY_STATUS(fift.interpret_file("main.fif", ""));
  return std::move(fift.config().source_lookup);
}
}  // namespace
td::Result<FiftOutput> mem_run_fift(std::string source, std::vector<std::string> args, std::string fift_dir) {
  std::stringstream ss;
  TRY_RESULT(source_lookup, create_source_lookup(std::move(source), true, true, true, true, true, true, true, fift_dir));
  TRY_RESULT_ASSIGN(source_lookup, run_fift(std::move(source_lookup), &ss, true, std::move(args)));
  FiftOutput res;
  res.source_lookup = std::move(source_lookup);
  res.output = ss.str();
  return std::move(res);
}
td::Result<FiftOutput> mem_run_fift(SourceLookup source_lookup, std::vector<std::string> args) {
  std::stringstream ss;
  TRY_RESULT_ASSIGN(source_lookup, run_fift(std::move(source_lookup), &ss, true, std::move(args)));
  FiftOutput res;
  res.source_lookup = std::move(source_lookup);
  res.output = ss.str();
  return std::move(res);
}
td::Result<fift::SourceLookup> create_mem_source_lookup(std::string main, std::string fift_dir, bool need_preamble,
                                                        bool need_asm, bool need_ton_util, bool need_lisp,
                                                        bool need_w3_code) {
  return create_source_lookup(std::move(main), need_preamble, need_asm, need_ton_util, need_lisp, need_w3_code, false, false,
                              fift_dir);
}

td::Result<td::Ref<vm::Cell>> compile_asm(td::Slice asm_code) {
  std::stringstream ss;
  std::string sb;
  sb.reserve(asm_code.size() + 100);
  sb.append("\"Asm.fif\" include\n <{\n");
  sb.append(asm_code.data(), asm_code.size());
  sb.append("\n}>c boc>B \"res\" B>file");

  TRY_RESULT(source_lookup, create_source_lookup(std::move(sb), true, true, true, false, false, false, false));
  TRY_RESULT(res, run_fift(std::move(source_lookup), &ss));
  TRY_RESULT(boc, res.read_file("res"));
  return vm::std_boc_deserialize(std::move(boc.data));
}

td::Result<CompiledProgramOutput> compile_asm_program(std::string&& program_code, const std::string& fift_dir) {
  std::string main_fif;
  main_fif.reserve(program_code.size() + 100);
  main_fif.append(program_code.data(), program_code.size());
  main_fif.append(R"( dup hashB B>X      $>B "hex" B>file)");   // write codeHashHex to a file
  main_fif.append(R"(     boc>B B>base64 $>B "boc" B>file)");   // write codeBoc64 to a file

  std::stringstream fift_output_stream;
  TRY_RESULT(source_lookup, create_source_lookup(std::move(main_fif), true, true, false, false, false, false, false, fift_dir));
  TRY_RESULT(res, run_fift(std::move(source_lookup), &fift_output_stream));

  TRY_RESULT(boc, res.read_file("boc"));
  TRY_RESULT(hex, res.read_file("hex"));

  return CompiledProgramOutput{
    std::move(program_code),
    std::move(boc.data),
    std::move(hex.data),
  };
}

}  // namespace fift
