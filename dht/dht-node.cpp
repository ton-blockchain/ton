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
#include "dht-node.hpp"
#include "keys/encryptor.h"

namespace ton {

namespace dht {

td::Status DhtNode::update(tl_object_ptr<ton_api::dht_node> obj) {
  if (version_ && obj->version_ <= version_) {
    return td::Status::Error(ErrorCode::notready, "too old version");
  }
  auto signature = std::move(obj->signature_);
  auto B = serialize_tl_object(obj, true);

  TRY_RESULT(pub, adnl::AdnlNodeIdFull::create(obj->id_));
  TRY_RESULT(addr_list, adnl::AdnlAddressList::create(std::move(obj->addr_list_)));

  if (!addr_list.public_only()) {
    return td::Status::Error(ErrorCode::notready, "dht node must have only public addresses");
  }
  if (!addr_list.size()) {
    return td::Status::Error(ErrorCode::notready, "dht node must have >0 addresses");
  }

  TRY_RESULT(E, pub.pubkey().create_encryptor());
  TRY_STATUS(E->check_signature(B.as_slice(), signature.as_slice()));

  id_ = pub;
  addr_list_ = addr_list;
  version_ = obj->version_;
  signature_ = td::SharedSlice(signature.as_slice());

  return td::Status::OK();
}

tl_object_ptr<ton_api::dht_nodes> DhtNodesList::tl() const {
  std::vector<tl_object_ptr<ton_api::dht_node>> L;
  for (auto &n : list_) {
    L.emplace_back(n.tl());
  }
  return create_tl_object<ton_api::dht_nodes>(std::move(L));
}

}  // namespace dht

}  // namespace ton
