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
#include "git.h"
#include "td/utils/JsonBuilder.h"
#include "fift/utils.h"
#include "td/utils/Status.h"
#include <sstream>

using namespace tolk;

static td::Result<std::string> compile_internal(char *config_json) {
  TRY_RESULT(input_json, td::json_decode(td::MutableSlice(config_json)))
  td::JsonObject& config = input_json.get_object();

  TRY_RESULT(opt_level, td::get_json_object_int_field(config, "optimizationLevel", true, 2));
  TRY_RESULT(stack_comments, td::get_json_object_bool_field(config, "withStackComments", true, false));
  TRY_RESULT(src_line_comments, td::get_json_object_bool_field(config, "withSrcLineComments", true, false));
  TRY_RESULT(entrypoint_filename, td::get_json_object_string_field(config, "entrypointFileName", false));
  TRY_RESULT(experimental_options, td::get_json_object_string_field(config, "experimentalOptions", true));

  G.settings.verbosity = 0;
  G.settings.optimization_level = std::max(0, opt_level);
  G.settings.stack_layout_comments = stack_comments;
  G.settings.tolk_src_as_line_comments = src_line_comments;
  if (!experimental_options.empty()) {
    G.settings.parse_experimental_options_cmd_arg(experimental_options.c_str());
  }

  std::ostringstream outs, errs;
  std::cout.rdbuf(outs.rdbuf());
  std::cerr.rdbuf(errs.rdbuf());
  int exit_code = tolk_proceed(entrypoint_filename);
  if (exit_code != 0) {
    return td::Status::Error("Tolk compilation error: " + errs.str());
  }

  TRY_RESULT(fift_res, fift::compile_asm_program(outs.str(), "/fiftlib/"));

  td::JsonBuilder result_json;
  auto obj = result_json.enter_object();
  obj("status", "ok");
  obj("fiftCode", fift_res.fiftCode);
  obj("codeBoc64", fift_res.codeBoc64);
  obj("codeHashHex", fift_res.codeHashHex);
  obj("stderr", errs.str().c_str());
  obj.leave();

  return result_json.string_builder().as_cslice().str();
}

/// Callback used to retrieve file contents from a "not file system". See tolk-js for implementation.
/// The callback must fill either destContents or destError.
/// The implementor must use malloc() for them and use free() after tolk_compile returns.
typedef void (*WasmFsReadCallback)(int kind, char const* data, char** destContents, char** destError);

static CompilerSettings::FsReadCallback wrap_wasm_read_callback(WasmFsReadCallback _readCallback) {
  return [_readCallback](CompilerSettings::FsReadCallbackKind kind, char const* data) -> td::Result<std::string> {
    char* destContents = nullptr;
    char* destError = nullptr;
    if (_readCallback) {
      _readCallback(static_cast<int>(kind), data, &destContents, &destError);
    }
    if (destContents) {
      return destContents;
    }
    if (destError) {
      return td::Status::Error(std::string(destError));
    }
    return td::Status::Error("Invalid callback from wasm");
  };
}

extern "C" {

const char* version() {
  td::JsonBuilder version_json = td::JsonBuilder();
  auto obj = version_json.enter_object();
  obj("tolkVersion", TOLK_VERSION);
  obj("tolkFiftLibCommitHash", GitMetadata::CommitSHA1());
  obj("tolkFiftLibCommitDate", GitMetadata::CommitDate());
  obj.leave();
  return strdup(version_json.string_builder().as_cslice().c_str());
}

const char *tolk_compile(char *config_json, WasmFsReadCallback callback) {
  G.settings.read_callback = wrap_wasm_read_callback(callback);

  td::Result<std::string> res = compile_internal(config_json);

  if (res.is_error()) {
    td::JsonBuilder error_res = td::JsonBuilder();
    auto obj = error_res.enter_object();
    obj("status", "error");
    obj("message", res.move_as_error().message().str());
    obj.leave();
    return strdup(error_res.string_builder().as_cslice().c_str());
  }

  std::string res_string = res.move_as_ok();
  return strdup(res_string.c_str());
}

} // extern "C"
