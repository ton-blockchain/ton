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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "adnl-proxy-types.hpp"
#include "tl-utils/tl-utils.hpp"
#include "auto/tl/ton_api.hpp"
#include "td/utils/overloaded.h"
#include "td/utils/Time.h"
#include "common/errorcode.h"

namespace ton {

namespace adnl {

td::BufferSlice AdnlProxyFast::encrypt(Packet packet) const {
  auto date = static_cast<td::uint32>(td::Clocks::system());
  auto signature = create_hash_tl_object<ton_api::adnl_proxyToFastHash>(
      packet.ip, packet.port, date, sha256_bits256(packet.data.as_slice()), shared_secret_);

  auto obj = create_serialize_tl_object<ton_api::adnl_proxyToFast>(packet.ip, packet.port, date, signature);
  td::BufferSlice res{32 + obj.size() + packet.data.size()};
  auto S = res.as_slice();
  S.copy_from(td::Bits256::zero().as_slice());
  S.remove_prefix(32);
  S.copy_from(obj.as_slice());
  S.remove_prefix(obj.size());
  S.copy_from(packet.data.as_slice());

  return res;
}

td::Result<AdnlProxy::Packet> AdnlProxyFast::decrypt(td::BufferSlice packet) const {
  if (packet.size() < 36) {
    return td::Status::Error(ErrorCode::protoviolation, "too short packet");
  }

  td::Bits256 v;
  v.as_slice().copy_from(packet.as_slice().truncate(32));
  if (!v.is_zero()) {
    return td::Status::Error(ErrorCode::protoviolation, "non-zero DST");
  }
  packet.confirm_read(32);

  TRY_RESULT(R, fetch_tl_prefix<ton_api::adnl_proxyToFast>(packet, true));

  if (R->date_ < td::Clocks::system() - 8) {
    return td::Status::Error(ErrorCode::protoviolation, "too old date");
  }

  auto signature = create_hash_tl_object<ton_api::adnl_proxyToFastHash>(
      R->ip_, R->port_, R->date_, sha256_bits256(packet.as_slice()), shared_secret_);
  if (signature != R->signature_) {
    return td::Status::Error(ErrorCode::protoviolation, "bad signature");
  }

  return Packet{static_cast<td::uint32>(R->ip_), static_cast<td::uint16>(R->port_), std::move(packet)};
}

td::Result<std::shared_ptr<AdnlProxy>> AdnlProxy::create(const ton_api::adnl_Proxy &proxy_type) {
  std::shared_ptr<AdnlProxy> R;
  ton_api::downcast_call(
      const_cast<ton_api::adnl_Proxy &>(proxy_type),
      td::overloaded([&](const ton_api::adnl_proxy_none &x) { R = std::make_shared<AdnlProxyNone>(); },
                     [&](const ton_api::adnl_proxy_fast &x) {
                       R = std::make_shared<AdnlProxyFast>(x.shared_secret_.as_slice());
                     }));
  return R;
}

}  // namespace adnl

}  // namespace ton
