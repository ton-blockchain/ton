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
#include "compiler-state.h"
#include "git.h"
#include "td/utils/JsonBuilder.h"
#include "fift/utils.h"
#include "td/utils/Status.h"
#include <sstream>

using namespace tolk;

td::Result<std::string> compile_internal(char *config_json) {
  TRY_RESULT(input_json, td::json_decode(td::MutableSlice(config_json)))
  td::JsonObject& config = input_json.get_object();

  TRY_RESULT(opt_level, td::get_json_object_int_field(config, "optimizationLevel", true, 2));
  TRY_RESULT(stdlib_tolk, td::get_json_object_string_field(config, "stdlibLocation", false));
  TRY_RESULT(stack_comments, td::get_json_object_bool_field(config, "withStackComments", true, false));
  TRY_RESULT(entrypoint_file_name, td::get_json_object_string_field(config, "entrypointFileName", false));

  G.settings.verbosity = 0;
  G.settings.optimization_level = std::max(0, opt_level);
  G.settings.stdlib_filename = stdlib_tolk;
  G.settings.stack_layout_comments = stack_comments;
  G.settings.entrypoint_filename = entrypoint_file_name;

  std::ostringstream outs, errs;
  std::cout.rdbuf(outs.rdbuf());
  std::cerr.rdbuf(errs.rdbuf());
  int tolk_res = tolk::tolk_proceed(entrypoint_file_name);
  if (tolk_res != 0) {
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
typedef void (*CStyleReadFileCallback)(int kind, char const* data, char** destContents, char** destError);

CompilerSettings::FsReadCallback wrapReadCallback(CStyleReadFileCallback _readCallback)
{
  CompilerSettings::FsReadCallback readCallback;
  if (_readCallback) {
    readCallback = [=](CompilerSettings::FsReadCallbackKind kind, char const* data) -> td::Result<std::string> {
      char* destContents = nullptr;
      char* destError = nullptr;
      _readCallback(static_cast<int>(kind), data, &destContents, &destError);
      if (!destContents && !destError) {
        return td::Status::Error("Callback not supported");
      }
      if (destContents) {
        return destContents;
      }
      return td::Status::Error(std::string(destError));
    };
  }
  return readCallback;
}

extern "C" {

const char* version() {
  auto version_json = td::JsonBuilder();
  auto obj = version_json.enter_object();
  obj("tolkVersion", tolk_version);
  obj("tolkFiftLibCommitHash", GitMetadata::CommitSHA1());
  obj("tolkFiftLibCommitDate", GitMetadata::CommitDate());
  obj.leave();
  return strdup(version_json.string_builder().as_cslice().c_str());
}

const char *tolk_compile(char *config_json, CStyleReadFileCallback callback) {
  G.settings.read_callback = wrapReadCallback(callback);

  td::Result<std::string> res = compile_internal(config_json);

  if (res.is_error()) {
    auto result = res.move_as_error();
    auto error_res = td::JsonBuilder();
    auto error_o = error_res.enter_object();
    error_o("status", "error");
    error_o("message", result.message().str());
    error_o.leave();
    return strdup(error_res.string_builder().as_cslice().c_str());
  }

  auto res_string = res.move_as_ok();

  return strdup(res_string.c_str());
}
}
