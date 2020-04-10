/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "adnl-test-loopback-implementation.h"

namespace ton {

namespace adnl {

AdnlAddressList TestLoopbackNetworkManager::generate_dummy_addr_list(bool empty) {
  auto obj = ton::create_tl_object<ton::ton_api::adnl_address_udp>(1, 1);
  auto objv = std::vector<ton::tl_object_ptr<ton::ton_api::adnl_Address>>();
  objv.push_back(std::move(obj));
  td::uint32 now = Adnl::adnl_start_time();
  auto addrR = ton::adnl::AdnlAddressList::create(
      ton::create_tl_object<ton::ton_api::adnl_addressList>(std::move(objv), empty ? 0 : now, empty ? 0 : now, 0, 0));
  addrR.ensure();
  return addrR.move_as_ok();
}

}  // namespace adnl

}  // namespace ton
