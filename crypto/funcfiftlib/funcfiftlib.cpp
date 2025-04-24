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
#include "func/func.h"
#include "git.h"
#include "td/utils/JsonBuilder.h"
#include "fift/utils.h"
#include "td/utils/base64.h"
#include "td/utils/Status.h"
#include <sstream>
#include <iomanip>
#include "vm/boc.h"

td::Result<std::string> compile_internal(char *config_json) {
  TRY_RESULT(input_json, td::json_decode(td::MutableSlice(config_json)))
  td::JsonObject& config = input_json.get_object();

  TRY_RESULT(opt_level, td::get_json_object_int_field(config, "optLevel", false));
  TRY_RESULT(sources_obj, td::get_json_object_field(config, "sources", td::JsonValue::Type::Array, false));

  auto &sources_arr = sources_obj.get_array();

  std::vector<std::string> sources;

  for (auto &item : sources_arr) {
    sources.push_back(item.get_string().str());
  }

  funC::opt_level = std::max(0, opt_level);
  funC::program_envelope = true;
  funC::asm_preamble = true;
  funC::verbosity = 0;
  funC::indent = 1;

  std::ostringstream outs, errs;
  int funC_res = funC::func_proceed(sources, outs, errs);
  if (funC_res != 0) {
    return td::Status::Error("FunC compilation error: " + errs.str());
  }

  TRY_RESULT(fift_res, fift::compile_asm_program(outs.str(), "/fiftlib/"));

  td::JsonBuilder result_json;
  auto obj = result_json.enter_object();
  obj("status", "ok");
  obj("fiftCode", std::move(fift_res.fiftCode));
  obj("codeBoc", std::move(fift_res.codeBoc64));
  obj("codeHashHex", std::move(fift_res.codeHashHex));
  obj.leave();

  return result_json.string_builder().as_cslice().str();
}

/// Callback used to retrieve additional source files or data.
///
/// @param _kind The kind of callback (a string).
/// @param _data The data for the callback (a string).
/// @param o_contents A pointer to the contents of the file, if found. Allocated via malloc().
/// @param o_error A pointer to an error message, if there is one. Allocated via malloc().
///
/// The callback implementor must use malloc() to allocate storage for
/// contents or error. The callback implementor must use free() to free
/// said storage after func_compile returns.
///
/// If the callback is not supported, *o_contents and *o_error must be set to NULL.
typedef void (*CStyleReadFileCallback)(char const* _kind, char const* _data, char** o_contents, char** o_error);

funC::ReadCallback::Callback wrapReadCallback(CStyleReadFileCallback _readCallback)
{
  funC::ReadCallback::Callback readCallback;
  if (_readCallback) {
    readCallback = [=](funC::ReadCallback::Kind _kind, char const* _data) -> td::Result<std::string> {
      char* contents_c = nullptr;
      char* error_c = nullptr;
      _readCallback(funC::ReadCallback::kindString(_kind).data(), _data, &contents_c, &error_c);
      if (!contents_c && !error_c) {
        return td::Status::Error("Callback not supported");
      }
      if (contents_c) {
        return contents_c;
      }
      return td::Status::Error(std::string(error_c));
    };
  }
  return readCallback;
}

extern "C" {

const char* version() {
  auto version_json = td::JsonBuilder();
  auto obj = version_json.enter_object();
  obj("funcVersion", funC::func_version);
  obj("funcFiftLibCommitHash", GitMetadata::CommitSHA1());
  obj("funcFiftLibCommitDate", GitMetadata::CommitDate());
  obj.leave();
  return strdup(version_json.string_builder().as_cslice().c_str());
}

const char *func_compile(char *config_json, CStyleReadFileCallback callback) {
  if (callback) {
    funC::read_callback = wrapReadCallback(callback);
  } else {
    funC::read_callback = funC::fs_read_callback;
  }

  auto res = compile_internal(config_json);

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
