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
#include "crypto/ellcurve/Ed25519.h"
#include "adnl/utils.hpp"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api_json.h"
#include "tl/tl_json.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/filesystem.h"
#include "keys/encryptor.h"
#include "keys/keys.hpp"

int main(int argc, char *argv[]) {
  ton::PrivateKey pk;
  ton::tl_object_ptr<ton::ton_api::adnl_addressList> addr_list;

  td::OptionsParser p;
  p.set_description("generate random id");

  std::string mode = "";

  std::string name = "id_ton";

  p.add_option('m', "mode", "sets mode (one of id/adnl/dht/keys/adnlid)", [&](td::Slice key) {
    mode = key.str();
    return td::Status::OK();
  });
  p.add_option('h', "help", "prints this help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
    return td::Status::OK();
  });
  p.add_option('n', "name", "path to save private keys to", [&](td::Slice arg) {
    name = arg.str();
    return td::Status::OK();
  });
  p.add_option('k', "key", "path to private key to import", [&](td::Slice key) {
    if (!pk.empty()) {
      return td::Status::Error("duplicate '-k' option");
    }

    TRY_RESULT_PREFIX(data, td::read_file_secure(key.str()), "failed to read private key: ");
    TRY_RESULT_PREFIX_ASSIGN(pk, ton::PrivateKey::import(data.as_slice()), "failed to import private key: ");
    return td::Status::OK();
  });
  p.add_option('a', "addr-list", "addr list to sign", [&](td::Slice key) {
    if (addr_list) {
      return td::Status::Error("duplicate '-a' option");
    }
    CHECK(!addr_list);

    td::BufferSlice bs(key);
    TRY_RESULT_PREFIX(as_json_value, td::json_decode(bs.as_slice()), "bad addr list JSON: ");
    TRY_STATUS_PREFIX(td::from_json(addr_list, as_json_value), "bad addr list TL: ");
    return td::Status::OK();
  });

  auto S = p.run(argc, argv);

  if (S.is_error()) {
    std::cerr << S.move_as_error().message().str() << std::endl;
    return 2;
  }

  if (mode.size() == 0) {
    std::cerr << "'--mode' option missing" << std::endl;
    return 2;
  }

  if (pk.empty()) {
    pk = ton::privkeys::Ed25519::random();
  }

  auto pub_key = pk.compute_public_key();
  auto short_key = pub_key.compute_short_id();

  if (mode == "id") {
    std::string v;
    v = td::json_encode<std::string>(td::ToJson(pk.tl()));
    std::cout << v << std::endl;
    v = td::json_encode<std::string>(td::ToJson(pub_key.tl()));
    std::cout << v << std::endl;
    v = td::json_encode<std::string>(td::ToJson(ton::adnl::AdnlNodeIdShort{short_key}.tl()));
    std::cout << v << std::endl;
  } else if (mode == "adnl") {
    if (!addr_list) {
      std::cerr << "'-a' option missing" << std::endl;
      return 2;
    }
    auto x = ton::create_tl_object<ton::ton_api::adnl_node>(pub_key.tl(), std::move(addr_list));
    auto e = pk.create_decryptor().move_as_ok();
    auto r = e->sign(ton::serialize_tl_object(x, true).as_slice()).move_as_ok();

    auto v = td::json_encode<std::string>(td::ToJson(x));
    std::cout << v << std::endl;
  } else if (mode == "dht") {
    if (!addr_list) {
      std::cerr << "'-a' option missing" << std::endl;
      return 2;
    }
    auto x = ton::create_tl_object<ton::ton_api::dht_node>(pub_key.tl(), std::move(addr_list), -1, td::BufferSlice());
    auto e = pk.create_decryptor().move_as_ok();
    auto r = e->sign(ton::serialize_tl_object(x, true).as_slice()).move_as_ok();
    x->signature_ = std::move(r);

    auto v = td::json_encode<std::string>(td::ToJson(x));
    std::cout << v << "\n";
  } else if (mode == "keys") {
    td::write_file(name, pk.export_as_slice()).ensure();
    td::write_file(name + ".pub", pub_key.export_as_slice().as_slice()).ensure();

    std::cout << short_key.bits256_value().to_hex() << " " << td::base64_encode(short_key.as_slice()) << std::endl;
  } else if (mode == "adnlid") {
    auto n = pk.compute_short_id();
    name = n.bits256_value().to_hex();
    td::write_file(name, pk.export_as_slice()).ensure();

    std::cout << name << " " << ton::adnl::AdnlNodeIdShort{n}.serialize() << std::endl;
  } else {
    std::cerr << "unknown mode " << mode;
    return 2;
  }
  return 0;
}
