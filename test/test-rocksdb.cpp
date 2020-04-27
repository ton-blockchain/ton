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
#include "td/db/RocksDb.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/path.h"
#include "td/utils/StringBuilder.h"

#include <iostream>

std::string dir = "stress-db";
int db_n = 20;
int key_n = 1000000;

std::string get_db_path(int i) {
  return PSTRING() << dir << TD_DIR_SLASH << "db-" << i;
}

void do_create_db() {
  td::mkdir(dir).ensure();
  for (int db_i = 0; db_i < db_n; db_i++) {
    LOG(ERROR) << "db_i=" << db_i;
    auto db = td::RocksDb::open(get_db_path(db_i)).move_as_ok();
    for (int key_i = 0; key_i < key_n; key_i++) {
      db.set(PSLICE() << key_i, PSLICE() << key_i);
    }
  }
}

void do_load_db() {
  static std::vector<td::RocksDb> dbs;
  for (int db_i = 0; db_i < db_n; db_i++) {
    LOG(ERROR) << "db_i=" << db_i;
    auto db = td::RocksDb::open(get_db_path(db_i)).move_as_ok();
    for (int key_i = 0; key_i < key_n; key_i++) {
      std::string value;
      db.get(PSLICE() << key_i, value).ensure();
    }
    dbs.push_back(std::move(db));
  }
}

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);
  td::set_default_failure_signal_handler().ensure();

  td::OptionsParser p;
  p.set_description("test basic adnl functionality");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
    return td::Status::OK();
  });
  bool create_db = false;
  p.add_option('c', "create", "create test db", [&] {
    create_db = true;
    return td::Status::OK();
  });

  auto res = p.run(argc, argv);
  LOG_IF(FATAL, res.is_error()) << res.error();

  if (create_db) {
    do_create_db();
  } else {
    do_load_db();
  }
  return 0;
}
