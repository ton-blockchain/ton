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
#include "parser/srcread.h"
#include "parser/lexer.h"
#include "parser/symtable.h"
#include <getopt.h>
#include <fstream>
#include "git.h"
#include "td/utils/JsonBuilder.h"


extern "C" {

const char* func_compile(char* config_json) {

  std::ostringstream outs, errs;
  char *json_ret;

  printf("config_json:\n %s\n", config_json);

  try {

    std::string json_str = std::string(config_json);

    auto res_conf = td::json_decode(json_str);
    if (res_conf.is_error()) {
      errs << res_conf.error().message().c_str();
      throw std::string("JSON decode error");
    }
    td::JsonValue conf_val = res_conf.move_as_ok();
    td::JsonObject &config_obj = conf_val.get_object();

    auto res_srcs = td::get_json_object_field(config_obj, "sources", td::JsonValue::Type::Array, false);
    if (res_srcs.is_error()) {
      errs << res_srcs.error().message().c_str();
      throw std::string("JSON decode error");
    }

    td::JsonValue srcs_val = res_srcs.move_as_ok();
    td::JsonArray &sources_arr = srcs_val.get_array();

    std::vector<std::string> sources;
    for (auto &src_obj : sources_arr) {
      auto src = src_obj.get_string().str();
      sources.push_back(src);
    }

    printf("sources:\n");
    for (auto &src : sources) {
      printf("%s\n", src.c_str());
    }

    auto res_opt_lvl = td::get_json_object_int_field(config_obj, "optLevel", false);
    if (res_opt_lvl.is_error()) {
      errs << res_opt_lvl.error().message().c_str();
      throw std::string("JSON decode error");
    }

    funC::opt_level = std::max(0, res_opt_lvl.move_as_ok());

    printf("opt_level: %d\n", funC::opt_level);

    funC::program_envelope = true;
    funC::verbosity = 0;
    funC::indent = 1;

    td::JsonBuilder result_json;
    auto result_obj = result_json.enter_object();

    if (funC::func_proceed(sources, outs, errs)) {
      throw std::string("func compiling");
    }

    auto func_res = td::JsonBuilder();
    auto func_o = func_res.enter_object();

    func_o("fiftCode", outs.str());
    func_o.leave();

    result_obj("funcResult", func_res.string_builder().as_cslice().c_str());
    outs.clear();
    errs.clear();

    result_obj.leave();
    json_ret = strdup(result_json.string_builder().as_cslice().c_str());
  }
  catch(std::string reason) {
      auto error_res = td::JsonBuilder();
      auto error_o = error_res.enter_object();
      error_o("status", "error");
      error_o("reason", reason);
      error_o("message", errs.str());
      error_o.leave();
      json_ret = strdup(error_res.string_builder().as_cslice().c_str());
  }
  catch(...) {
     printf("undefined exeption type\n");
  }

  return json_ret;

}

}
