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

#include "adnl-node-id.hpp"
#include "adnl-address-list.h"

namespace ton {

namespace adnl {

class AdnlAddressUdp : public AdnlAddressImpl {
 private:
  td::uint32 ip_;
  td::uint16 port_;

 public:
  explicit AdnlAddressUdp(const ton_api::adnl_address_udp &obj);

  AdnlAddressUdp(td::uint32 ip, td::uint16 port) : ip_(ip), port_(port) {
  }

  AdnlAddressUdp *make_copy() const override {
    return new AdnlAddressUdp{ip_, port_};
  }

  bool is_public() const override {
    return true;
  }
  td::uint32 serialized_size() const override {
    return 12;
  }

  tl_object_ptr<ton_api::adnl_Address> tl() const override {
    return create_tl_object<ton_api::adnl_address_udp>(ip_, port_);
  }
  td::actor::ActorOwn<AdnlNetworkConnection> create_connection(
      td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<Adnl> adnl,
      std::unique_ptr<AdnlNetworkConnection::Callback> callback) const override;
};

class AdnlAddressUdp6 : public AdnlAddressImpl {
 private:
  td::Bits128 ip_;
  td::uint16 port_;

 public:
  explicit AdnlAddressUdp6(const ton_api::adnl_address_udp6 &obj);

  AdnlAddressUdp6(td::Bits128 ip, td::uint16 port) : ip_(ip), port_(port) {
  }

  AdnlAddressUdp6 *make_copy() const override {
    return new AdnlAddressUdp6{ip_, port_};
  }

  bool is_public() const override {
    return true;
  }
  td::uint32 serialized_size() const override {
    return 12;
  }

  tl_object_ptr<ton_api::adnl_Address> tl() const override {
    return create_tl_object<ton_api::adnl_address_udp6>(ip_, port_);
  }
  td::actor::ActorOwn<AdnlNetworkConnection> create_connection(
      td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<Adnl> adnl,
      std::unique_ptr<AdnlNetworkConnection::Callback> callback) const override;
};

class AdnlAddressTunnel : public AdnlAddressImpl {
 private:
  AdnlNodeIdShort adnl_id_;
  PublicKey pub_key_;

 public:
  explicit AdnlAddressTunnel(const ton_api::adnl_address_tunnel &obj);

  AdnlAddressTunnel(AdnlNodeIdShort adnl_id, PublicKey pub_key)
      : adnl_id_(std::move(adnl_id)), pub_key_(std::move(pub_key)) {
  }

  AdnlAddressTunnel *make_copy() const override {
    return new AdnlAddressTunnel{*this};
  }

  bool is_public() const override {
    return false;
  }
  td::uint32 serialized_size() const override {
    return 4 + 32 + pub_key_.serialized_size();
  }

  tl_object_ptr<ton_api::adnl_Address> tl() const override {
    return create_tl_object<ton_api::adnl_address_tunnel>(adnl_id_.bits256_value(), pub_key_.tl());
  }
  td::actor::ActorOwn<AdnlNetworkConnection> create_connection(
      td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<Adnl> adnl,
      std::unique_ptr<AdnlNetworkConnection::Callback> callback) const override;
};

class AdnlAddressReverse : public AdnlAddressImpl {
 public:
  AdnlAddressReverse *make_copy() const override {
    return new AdnlAddressReverse();
  }
  bool is_public() const override {
    return true;
  }
  td::uint32 serialized_size() const override {
    return 4;
  }
  tl_object_ptr<ton_api::adnl_Address> tl() const override {
    return create_tl_object<ton_api::adnl_address_reverse>();
  }
  td::actor::ActorOwn<AdnlNetworkConnection> create_connection(
      td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<Adnl> adnl,
      std::unique_ptr<AdnlNetworkConnection::Callback> callback) const override {
    LOG(ERROR) << "Cannot create connection for AdnlAddressReverse";
    return {};
  }
  bool is_reverse() const override {
    return true;
  }
};

}  // namespace adnl

}  // namespace ton
