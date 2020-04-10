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
#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <cassert>
#include "td/utils/OptionsParser.h"
#include "keys/encryptor.h"
#include "auto/tl/ton_api_json.h"
#include "td/utils/filesystem.h"
#include "common/io.hpp"
#include "common/checksum.h"
#include "tl/tl_json.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api_json.h"

int main(int argc, char *argv[]) {
  std::string in_f;
  std::string out_f;
  bool reverse_ = false;

  td::OptionsParser p;
  p.set_description("json2tlo");

  p.add_option('i', "in", "input", [&](td::Slice key) {
    in_f = key.str();
    return td::Status::OK();
  });
  p.add_option('o', "out", "output", [&](td::Slice key) {
    out_f = key.str();
    return td::Status::OK();
  });
  p.add_option('r', "reverse", "read tlo, print json", [&]() {
    reverse_ = !reverse_;
    return td::Status::OK();
  });
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
    return td::Status::OK();
  });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    std::cerr << S.move_as_error().message().str() << std::endl;
    return 2;
  }

  if (in_f.size() == 0) {
    std::cerr << "missing --in option" << std::endl;
    return 2;
  }

  auto R = [&]() -> td::Status {
    TRY_RESULT(in_data, td::read_file(in_f));
    td::BufferSlice out_data;
    if (!reverse_) {
      TRY_RESULT(j, td::json_decode(in_data.as_slice()));
      ton::tl_object_ptr<ton::ton_api::Object> s_data;
      TRY_STATUS(td::from_json(s_data, j));

      out_data = serialize_tl_object(s_data, true);
    } else {
      TRY_RESULT(X, ton::fetch_tl_object<ton::ton_api::Object>(std::move(in_data), true));
      out_data = td::json_encode<td::BufferSlice>(td::ToJson(*X.get()));
    }
    auto hash = td::sha256_bits256(out_data.as_slice());

    if (out_f.size() == 0) {
      if (!reverse_) {
        out_f = hash.to_hex();
      }
    }
    if (out_f.size() > 0) {
      write_file(out_f, out_data.as_slice()).ensure();
    } else {
      std::cout << out_data.as_slice().str() << std::endl;
    }
    return td::Status::OK();
  }();

  if (R.is_error()) {
    std::cerr << R.message().str() << std::endl;
    return 2;
  }

  return 0;
}

