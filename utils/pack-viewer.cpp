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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <cassert>
#include "td/utils/OptionsParser.h"

#include "validator/db/package.hpp"
#include "validator/db/fileref.hpp"

void run(std::string filename) {
  auto R = ton::Package::open(filename, true, false);
  if (R.is_error()) {
    std::cerr << "failed to open archive '" << filename << "': " << R.move_as_error().to_string();
    std::_Exit(2);
  }
  auto p = R.move_as_ok();

  p.iterate([&](std::string filename, td::BufferSlice data, td::uint64 offset) -> bool {
    auto E = ton::validator::FileReference::create(filename);
    if (E.is_error()) {
      std::cout << "bad filename\n";
    } else {
      std::cout << filename << " " << data.size() << "\n";
    }
    return true;
  });
}

int main(int argc, char **argv) {
  run(argv[1]);
  return 0;
}

