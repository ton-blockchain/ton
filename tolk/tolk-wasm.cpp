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
#include "compiler-settings.h"
#include "git.h"
#include "td/utils/JsonBuilder.h"
#include "json-output.h"
#include "fift/utils.h"
#include "td/utils/Status.h"
#include <mutex>
#include <sstream>

using namespace tolk;

static void output_errors_all_human_readable(JsonPrettyOutput& json, const std::vector<ThrownParseError>& errors) {
  constexpr int CONSOLE_ERROR_LIMIT = 20;
  int shown = 0;

  std::ostringstream concat;
  for (const ThrownParseError& error : errors) {
    if (shown >= CONSOLE_ERROR_LIMIT) break;
    if (shown++) concat << std::endl;  // separator between errors
    error.output_to_console(concat);
  }
  json.key_value("message", concat.str());
}

static void output_errors_as_json_array(JsonPrettyOutput& json, const std::vector<ThrownParseError>& errors) {
  constexpr int JSON_ERROR_LIMIT = 50;
  int shown = 0;

  json.start_array("errors");
  for (const ThrownParseError& error : errors) {
    if (shown >= JSON_ERROR_LIMIT) break;
    error.output_to_json(json);
    shown++;
  }
  json.end_array();
}

static td::Result<std::string> compile_internal(char *config_json) {
  TRY_RESULT(input_json, td::json_decode(td::MutableSlice(config_json)))
  td::JsonObject& config = input_json.get_object();

  TRY_RESULT(opt_level, config.get_optional_int_field("optimizationLevel", 2));
  TRY_RESULT(stack_comments, config.get_optional_bool_field("withStackComments", false));
  TRY_RESULT(src_line_comments, config.get_optional_bool_field("withSrcLineComments", false));
  TRY_RESULT(entrypoint_filename, config.get_required_string_field("entrypointFileName"));
  TRY_RESULT(show_errors_as_json, config.get_optional_bool_field("jsonErrors", false));
  TRY_RESULT(check_only_no_output, config.get_optional_bool_field("checkOnly", false));
  TRY_RESULT(allow_no_entrypoint, config.get_optional_bool_field("allowNoEntrypoint", false));
  // note that `pathMappings` are handled on a client-side (in tolk-js) only

  G_settings.verbosity = 0;
  G_settings.optimization_level = std::max(0, opt_level);
  G_settings.stack_layout_comments = stack_comments;
  G_settings.tolk_src_as_line_comments = src_line_comments;
  G_settings.show_errors_as_json = show_errors_as_json;
  G_settings.check_only_no_output = check_only_no_output;
  G_settings.allow_no_entrypoint = allow_no_entrypoint;

  std::ostringstream errs;
  std::cerr.rdbuf(errs.rdbuf());

  TolkCompilationResult result = tolk_proceed(entrypoint_filename);
  if (!result.fatal_msg.empty()) {
    // no location or errors in json, just a message "fatal", something unexpected happened
    return td::Status::Error(td::Slice(result.fatal_msg.c_str()));
  }
  if (!result.errors.empty()) {
    // regular response with a list of compilation errors
    std::ostringstream result_json_str;
    JsonPrettyOutput json(result_json_str);
    json.start_object();
    json.key_value("status", "error");
    if (G_settings.show_errors_as_json) {   // { status: "error", errors: [...] }
      output_errors_as_json_array(json, result.errors);
    } else {                                // { status: "error", message: "one formatted multiline string" }
      output_errors_all_human_readable(json, result.errors);
    }
    json.end_object();
    return result_json_str.str();
  }

  // for IDE in background: all checks passed, skip codegen
  if (G_settings.check_only_no_output) {
    std::ostringstream result_json_str;
    JsonPrettyOutput json(result_json_str);
    json.start_object();
    json.key_value("status", "ok");
    json.key_value("stderr", errs.str());
    json.end_object();
    return result_json_str.str();
  }

  // an external wrapper should handle not only `@stdlib/`, but also `@fiftlib/`;
  // in tolk-js particularly, .fif files are embedded into distribution, next to tolk-stdlib/ folder
  TRY_RESULT(fift_fif, G_settings.read_callback(CompilerSettings::FsReadCallbackKind::ReadFile, "@fiftlib/Fift.fif", G_settings.callback_payload));
  TRY_RESULT(asm_fif,  G_settings.read_callback(CompilerSettings::FsReadCallbackKind::ReadFile, "@fiftlib/Asm.fif", G_settings.callback_payload));

  // Fift is not thread-safe, so we invoke `compile_asm_program()` with a mutex.
  // This is acceptable, because Fift compilation is very fast compared to tolk_proceed (which is fully parallel).
  static std::mutex fift_mutex;
  std::lock_guard<std::mutex> fift_lock(fift_mutex);

  td::Result<fift::CompiledProgramOutput> fift_result = fift::compile_asm_program(result.fift_code, std::move(fift_fif), std::move(asm_fif));
  if (fift_result.is_error()) {
    std::ostringstream result_json_str;
    JsonPrettyOutput json(result_json_str);
    json.start_object();
    json.key_value("status", "error");
    json.key_value("message", fift_result.move_as_error().message().str());
    json.key_value("fiftCode", result.fift_code);     // return fiftCode even if Fift couldn't process it
    json.key_value("tolkVersion", JsonPrettyOutput::Unescaped{TOLK_VERSION});
    json.end_object();
    return result_json_str.str();
  }
  fift::CompiledProgramOutput fift_res = fift_result.move_as_ok();

  std::ostringstream result_json_str;
  JsonPrettyOutput json(result_json_str);
  json.start_object();
  json.key_value("status", "ok");
  json.key_value("fiftCode", result.fift_code);
  json.key_value("codeBoc64", JsonPrettyOutput::Unescaped{fift_res.codeBoc64});
  json.key_value("codeHashHex", JsonPrettyOutput::Unescaped{fift_res.codeHashHex});
  json.key_value("tolkVersion", JsonPrettyOutput::Unescaped{TOLK_VERSION});
  json.key_value("stderr", errs.str());
  json.end_object();

  return result_json_str.str();
}

/// Callback used to retrieve file contents from a "not file system". See tolk-js for implementation.
/// The callback must fill either destContents or destError.
/// The implementor must use malloc() for them and use free() after tolk_compile returns.
/// callback_payload is an opaque pointer passed through from tolk_compile(), for caller-side identification.
typedef void (*WasmFsReadCallback)(int kind, char const* data, char** destContents, char** destError, void* callback_payload);

static CompilerSettings::FsReadCallback wrap_wasm_read_callback(WasmFsReadCallback _readCallback) {
  return [_readCallback](CompilerSettings::FsReadCallbackKind kind, char const* data, void* callback_payload) -> td::Result<std::string> {
    char* destContents = nullptr;
    char* destError = nullptr;
    if (_readCallback) {
      _readCallback(static_cast<int>(kind), data, &destContents, &destError, callback_payload);
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

const char* tolk_version() {
  std::ostringstream result_json_str;
  JsonPrettyOutput json(result_json_str);
  json.start_object();
  json.key_value("tolkVersion", JsonPrettyOutput::Unescaped{TOLK_VERSION});
  json.key_value("tolkFiftLibCommitHash", JsonPrettyOutput::Unescaped{GitMetadata::CommitSHA1()});
  json.key_value("tolkFiftLibCommitDate", JsonPrettyOutput::Unescaped{GitMetadata::CommitDate()});
  json.end_object();
  return strdup(result_json_str.str().c_str());
}

const char *tolk_compile(char *config_json, WasmFsReadCallback callback, void* callback_payload) {
  G_settings.read_callback = wrap_wasm_read_callback(callback);
  G_settings.callback_payload = callback_payload;

  td::Result<std::string> res = compile_internal(config_json);

  if (res.is_error()) {
    // it's an error of TRY_RESULT macro
    std::ostringstream result_json_str;
    JsonPrettyOutput json(result_json_str);
    json.start_object();
    json.key_value("status", "error");
    json.key_value("message", res.move_as_error().message().str());
    json.end_object();
    return strdup(result_json_str.str().c_str());
  }

  std::string json_string = res.move_as_ok();
  return strdup(json_string.c_str());
}

} // extern "C"
