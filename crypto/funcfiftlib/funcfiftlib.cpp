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
#include <sstream>
#include <iomanip>

std::string escape_json(const std::string &s) {
  std::ostringstream o;
  for (auto c = s.cbegin(); c != s.cend(); c++) {
    switch (*c) {
      case '"': o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\b': o << "\\b"; break;
      case '\f': o << "\\f"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
      default:
        if ('\x00' <= *c && *c <= '\x1f') {
          o << "\\u"
            << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(*c);
        } else {
          o << *c;
        }
    }
  }
  return o.str();
}

td::Result<std::string> compile_internal(char *config_json) {
  TRY_RESULT(input_json, td::json_decode(td::MutableSlice(config_json)))
  auto &obj = input_json.get_object();

  TRY_RESULT(opt_level, td::get_json_object_int_field(obj, "optLevel", false));
  TRY_RESULT(sources_obj, td::get_json_object_field(obj, "sources", td::JsonValue::Type::Array, false));

  auto &sources_arr = sources_obj.get_array();

  std::vector<std::string> sources;

  for (auto &item : sources_arr) {
    sources.push_back(item.get_string().str());
  }

  funC::opt_level = std::max(0, opt_level);
  funC::program_envelope = true;
  funC::verbosity = 0;
  funC::indent = 1;

  std::ostringstream outs, errs;
  auto compile_res = funC::func_proceed(sources, outs, errs);

  if (compile_res != 0) {
    return td::Status::Error(std::string("Func compilation error: ") + errs.str());
  }

  TRY_RESULT(code_cell, fift::compile_asm(outs.str(), "/fiftlib/", false));
  TRY_RESULT(boc, vm::std_boc_serialize(code_cell));

  td::JsonBuilder result_json;
  auto result_obj = result_json.enter_object();
  result_obj("status", "ok");
  result_obj("codeBoc", td::base64_encode(boc));
  result_obj("fiftCode", escape_json(outs.str()));
  result_obj.leave();

  outs.clear();
  errs.clear();

  return result_json.string_builder().as_cslice().str();
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

const char *func_compile(char *config_json) {
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
