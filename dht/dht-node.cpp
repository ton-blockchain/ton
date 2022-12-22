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

td::Status DhtNode::update(tl_object_ptr<ton_api::dht_node> obj, td::int32 our_network_id) {
  if (version_ && obj->version_ <= version_) {
    return td::Status::Error(ErrorCode::notready, "too old version");
  }
  td::BufferSlice signature;
  td::int32 network_id = -1;
  if (obj->signature_.size() == 64) {
    signature = std::move(obj->signature_);
  } else if (obj->signature_.size() == 64 + 4) {
    signature = td::BufferSlice{obj->signature_.as_slice().remove_prefix(4)};
    network_id = *(td::uint32 *)obj->signature_.as_slice().remove_suffix(64).data();
  } else {
    return td::Status::Error(ErrorCode::notready, "invalid length of signature");
  }
  if (network_id != our_network_id && network_id != -1 && our_network_id != -1) {
    // Remove (network_id != -1 && our_network_id != -1) after network update
    return td::Status::Error(ErrorCode::notready, PSTRING() << "wrong network id (expected " << our_network_id
                                                            << ", found " << network_id << ")");
  }
  TRY_RESULT(pub, adnl::AdnlNodeIdFull::create(obj->id_));
  TRY_RESULT(addr_list, adnl::AdnlAddressList::create(std::move(obj->addr_list_)));
  if (!addr_list.public_only()) {
    return td::Status::Error(ErrorCode::notready, "dht node must have only public addresses");
  }
  if (!addr_list.size()) {
    return td::Status::Error(ErrorCode::notready, "dht node must have >0 addresses");
  }
  DhtNode new_node{std::move(pub), std::move(addr_list), obj->version_, network_id, std::move(signature)};
  TRY_STATUS(new_node.check_signature());

  *this = std::move(new_node);
  return td::Status::OK();
}

td::Status DhtNode::check_signature() const {
  TRY_RESULT(enc, id_.pubkey().create_encryptor());
  auto node2 = clone();
  node2.signature_ = {};
  TRY_STATUS_PREFIX(enc->check_signature(serialize_tl_object(node2.tl(), true).as_slice(), signature_.as_slice()),
                    "bad node signature: ");
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
