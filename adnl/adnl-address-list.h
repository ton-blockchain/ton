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

#include "adnl-network-manager.h"
#include "crypto/common/refcnt.hpp"

namespace ton {

namespace adnl {

class Adnl;

class AdnlAddressImpl : public td::CntObject {
 public:
  using Hash = td::Bits256;
  virtual ~AdnlAddressImpl() = default;

  virtual Hash get_hash() const;
  virtual bool is_public() const = 0;
  virtual td::uint32 serialized_size() const = 0;
  virtual tl_object_ptr<ton_api::adnl_Address> tl() const = 0;
  virtual td::actor::ActorOwn<AdnlNetworkConnection> create_connection(
      td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<Adnl> adnl,
      std::unique_ptr<AdnlNetworkConnection::Callback> callback) const = 0;
  virtual bool is_reverse() const {
    return false;
  }

  static td::Ref<AdnlAddressImpl> create(const tl_object_ptr<ton_api::adnl_Address> &addr);
};

using AdnlAddress = td::Ref<AdnlAddressImpl>;

class AdnlAddressList {
 private:
  AdnlAddressList(const tl_object_ptr<ton_api::adnl_addressList> &addrs);

  td::int32 version_;
  td::int32 reinit_date_;
  td::int32 priority_;
  td::int32 expire_at_;
  std::vector<AdnlAddress> addrs_;
  bool has_reverse_{false};

 public:
  static constexpr td::uint32 max_serialized_size() {
    return 128;
  }

  const auto &addrs() const {
    return addrs_;
  }
  auto version() const {
    return version_;
  }
  auto reinit_date() const {
    return reinit_date_;
  }
  auto priority() const {
    return priority_;
  }
  auto expire_at() const {
    return expire_at_;
  }
  void set_version(td::uint32 version) {
    version_ = version;
  }
  void set_reinit_date(td::int32 date) {
    reinit_date_ = date;
  }
  void set_expire_at(td::int32 date) {
    expire_at_ = date;
  }
  bool empty() const {
    return version_ == -1;
  }
  void add_addr(AdnlAddress addr) {
    addrs_.push_back(addr);
  }
  void update(td::IPAddress addr);
  bool public_only() const;
  td::uint32 size() const {
    return td::narrow_cast<td::uint32>(addrs_.size());
  }
  td::uint32 serialized_size() const;
  tl_object_ptr<ton_api::adnl_addressList> tl() const;
  AdnlAddressList() : version_{-1}, reinit_date_{0}, priority_{0}, expire_at_{0} {
  }

  static td::Result<AdnlAddressList> create(const tl_object_ptr<ton_api::adnl_addressList> &addr_list);
  td::Status add_udp_address(td::IPAddress addr);

  void set_reverse(bool x = true) {
    has_reverse_ = x;
  }
  bool has_reverse() const {
    return has_reverse_;
  }
};

}  // namespace adnl

}  // namespace ton
