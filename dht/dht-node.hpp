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
#pragma once

#include "adnl/adnl-node-id.hpp"
#include "adnl/adnl-address-list.hpp"

#include "dht-types.h"

namespace ton {

namespace dht {

class DhtNode {
 private:
  adnl::AdnlNodeIdFull id_;
  adnl::AdnlAddressList addr_list_;
  td::int32 version_{0};
  td::SharedSlice signature_;

 public:
  DhtNode() {
  }
  DhtNode(adnl::AdnlNodeIdFull id, adnl::AdnlAddressList addr_list, td::int32 version, td::BufferSlice signature)
      : id_(id), addr_list_(addr_list), version_(version), signature_(signature.as_slice()) {
  }
  DhtNode(adnl::AdnlNodeIdFull id, adnl::AdnlAddressList addr_list, td::int32 version, td::SharedSlice signature)
      : id_(id), addr_list_(addr_list), version_(version), signature_(std::move(signature)) {
  }
  static td::Result<DhtNode> create(tl_object_ptr<ton_api::dht_node> obj) {
    if (obj->version_ == 0) {
      return td::Status::Error(ErrorCode::protoviolation, "zero version");
    }
    DhtNode n;
    TRY_STATUS(n.update(std::move(obj)));
    return std::move(n);
  }
  td::Status update(tl_object_ptr<ton_api::dht_node> obj);
  DhtKeyId get_key() const {
    CHECK(!id_.empty());
    return DhtKeyId{id_.compute_short_id()};
  }

  adnl::AdnlNodeIdFull adnl_id() const {
    return id_;
  }
  adnl::AdnlAddressList addr_list() const {
    return addr_list_;
  }
  td::int32 version() const {
    return version_;
  }

  tl_object_ptr<ton_api::dht_node> tl() const {
    return create_tl_object<ton_api::dht_node>(id_.tl(), addr_list_.tl(), version_, signature_.clone_as_buffer_slice());
  }
  DhtNode clone() const {
    return DhtNode{id_, addr_list_, version_, signature_.clone()};
  }
};

class DhtNodesList {
 public:
  DhtNodesList() {
  }
  DhtNodesList(tl_object_ptr<ton_api::dht_nodes> R) {
    for (auto &n : R->nodes_) {
      auto N = DhtNode::create(std::move(n));
      if (N.is_ok()) {
        list_.emplace_back(N.move_as_ok());
      } else {
        LOG(WARNING) << "bad dht node: " << N.move_as_error();
      }
    }
  }

  void push_back(DhtNode node) {
    list_.emplace_back(std::move(node));
  }

  tl_object_ptr<ton_api::dht_nodes> tl() const;
  std::vector<DhtNode> &list() {
    return list_;
  }
  const std::vector<DhtNode> &list() const {
    return list_;
  }
  td::uint32 size() const {
    return static_cast<td::uint32>(list_.size());
  }

 private:
  std::vector<DhtNode> list_;
};

}  // namespace dht

}  // namespace ton
