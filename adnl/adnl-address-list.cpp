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
#include "adnl-address-list.hpp"
#include "adnl-peer-table.h"
#include "auto/tl/ton_api.hpp"
#include "td/utils/overloaded.h"
#include "td/net/UdpServer.h"
#include "keys/encryptor.h"

namespace ton {

namespace adnl {

class AdnlNetworkConnectionUdp : public AdnlNetworkConnection {
 public:
  void send(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::uint32 priority, td::BufferSlice message) override;
  bool is_alive() const override {
    return true;
  }
  bool is_active() const override {
    return true;
  }
  void start_up() override {
    callback_->on_change_state(true);
  }
  void get_ip_str(td::Promise<td::string> promise) override {
    promise.set_value(PSTRING() << addr_.get_ip_str().str() << ":" << addr_.get_port());
  }

  AdnlNetworkConnectionUdp(td::actor::ActorId<AdnlNetworkManager> network_manager, td::uint32 ip, td::uint16 port,
                           std::unique_ptr<AdnlNetworkConnection::Callback> callback);
  AdnlNetworkConnectionUdp(td::actor::ActorId<AdnlNetworkManager> network_manager, td::Bits128 ip, td::uint16 port,
                           std::unique_ptr<AdnlNetworkConnection::Callback> callback);

 private:
  td::actor::ActorId<AdnlNetworkManager> network_manager_;
  td::IPAddress addr_;
  std::unique_ptr<AdnlNetworkConnection::Callback> callback_;
};

class AdnlNetworkConnectionTunnel : public AdnlNetworkConnection {
 public:
  void send(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::uint32 priority, td::BufferSlice message) override {
    if (!encryptor_) {
      VLOG(ADNL_INFO) << "tunnel: message [" << src << "->" << dst << "to bad tunnel. dropping";
      return;
    }
    auto dataR = encryptor_->encrypt(message.as_slice());
    if (dataR.is_error()) {
      VLOG(ADNL_INFO) << "tunnel: message [" << src << "->" << dst << ": failed to encrypt: " << dataR.move_as_error();
      return;
    }
    auto data = dataR.move_as_ok();
    td::BufferSlice enc_message{data.size() + 32};
    auto S = enc_message.as_slice();
    S.copy_from(pub_key_hash_.as_slice());
    S.remove_prefix(32);
    S.copy_from(data.as_slice());
    td::actor::send_closure(adnl_, &Adnl::send_message_ex, src, adnl_id_, std::move(enc_message),
                            Adnl::SendFlags::direct_only);
  }
  bool is_alive() const override {
    return ready_.load(std::memory_order_consume);
  }
  bool is_active() const override {
    return ready_.load(std::memory_order_consume);
  }
  void start_up() override {
    auto R = pub_key_.create_encryptor();
    if (R.is_error()) {
      VLOG(ADNL_INFO) << "tunnel: bad public key: " << R.move_as_error();
      return;
    }
    encryptor_ = R.move_as_ok();
    pub_key_hash_ = pub_key_.compute_short_id();
    //ready_.store(true, std::memory_order_release);
  }
  void get_ip_str(td::Promise<td::string> promise) override {
    promise.set_value("tunnel");
  }

  AdnlNetworkConnectionTunnel(td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<Adnl> adnl,
                              adnl::AdnlNodeIdShort adnl_id, PublicKey pubkey,
                              std::unique_ptr<AdnlNetworkConnection::Callback> callback);

 private:
  td::actor::ActorId<AdnlNetworkManager> network_manager_;
  td::actor::ActorId<Adnl> adnl_;
  AdnlNodeIdShort adnl_id_;
  PublicKey pub_key_;
  PublicKeyHash pub_key_hash_;
  std::unique_ptr<Encryptor> encryptor_;
  std::atomic<bool> ready_{false};
  std::unique_ptr<AdnlNetworkConnection::Callback> callback_;
};

void AdnlNetworkConnectionUdp::send(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::uint32 priority,
                                    td::BufferSlice message) {
  LOG_CHECK(message.size() <= AdnlNetworkManager::get_mtu()) << "dst=" << addr_ << " size=" << message.size();
  td::actor::send_closure(network_manager_, &AdnlNetworkManager::send_udp_packet, src, dst, addr_, priority,
                          std::move(message));
}

AdnlNetworkConnectionUdp::AdnlNetworkConnectionUdp(td::actor::ActorId<AdnlNetworkManager> network_manager,
                                                   td::uint32 ip, td::uint16 port,
                                                   std::unique_ptr<AdnlNetworkConnection::Callback> callback)
    : network_manager_(network_manager), callback_(std::move(callback)) {
  addr_.init_host_port(td::IPAddress::ipv4_to_str(ip), port).ensure();
}

AdnlNetworkConnectionUdp::AdnlNetworkConnectionUdp(td::actor::ActorId<AdnlNetworkManager> network_manager,
                                                   td::Bits128 ip, td::uint16 port,
                                                   std::unique_ptr<AdnlNetworkConnection::Callback> callback)
    : network_manager_(network_manager), callback_(std::move(callback)) {
  addr_.init_host_port(td::IPAddress::ipv6_to_str(ip.as_slice()), port).ensure();
}

AdnlNetworkConnectionTunnel::AdnlNetworkConnectionTunnel(td::actor::ActorId<AdnlNetworkManager> network_manager,
                                                         td::actor::ActorId<Adnl> adnl, adnl::AdnlNodeIdShort adnl_id,
                                                         PublicKey pubkey,
                                                         std::unique_ptr<AdnlNetworkConnection::Callback> callback)
    : network_manager_(std::move(network_manager))
    , adnl_(std::move(adnl))
    , adnl_id_(adnl_id)
    , pub_key_(std::move(pubkey))
    , callback_(std::move(callback)) {
}

AdnlAddressImpl::Hash AdnlAddressImpl::get_hash() const {
  return get_tl_object_sha_bits256(tl());
}

td::actor::ActorOwn<AdnlNetworkConnection> AdnlAddressUdp::create_connection(
    td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<Adnl> adnl,
    std::unique_ptr<AdnlNetworkConnection::Callback> callback) const {
  return td::actor::create_actor<AdnlNetworkConnectionUdp>("udpconn", network_manager, ip_, port_, std::move(callback));
}

AdnlAddressUdp::AdnlAddressUdp(const ton_api::adnl_address_udp &obj) {
  ip_ = obj.ip_;
  port_ = static_cast<td::uint16>(obj.port_);
}

td::actor::ActorOwn<AdnlNetworkConnection> AdnlAddressUdp6::create_connection(
    td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<Adnl> adnl,
    std::unique_ptr<AdnlNetworkConnection::Callback> callback) const {
  return td::actor::create_actor<AdnlNetworkConnectionUdp>("udpconn", network_manager, ip_, port_, std::move(callback));
}

AdnlAddressUdp6::AdnlAddressUdp6(const ton_api::adnl_address_udp6 &obj) {
  ip_ = obj.ip_;
  port_ = static_cast<td::uint16>(obj.port_);
}

td::actor::ActorOwn<AdnlNetworkConnection> AdnlAddressTunnel::create_connection(
    td::actor::ActorId<AdnlNetworkManager> network_manager, td::actor::ActorId<Adnl> adnl,
    std::unique_ptr<AdnlNetworkConnection::Callback> callback) const {
  return td::actor::create_actor<AdnlNetworkConnectionTunnel>("tunnelconn", network_manager, adnl, adnl_id_, pub_key_,
                                                              std::move(callback));
}
AdnlAddressTunnel::AdnlAddressTunnel(const ton_api::adnl_address_tunnel &obj) {
  adnl_id_ = AdnlNodeIdShort{obj.to_};
  pub_key_ = ton::PublicKey{obj.pubkey_};
}

td::Ref<AdnlAddressImpl> AdnlAddressImpl::create(const tl_object_ptr<ton_api::adnl_Address> &addr) {
  td::Ref<AdnlAddressImpl> res = td::Ref<AdnlAddressImpl>{};
  ton_api::downcast_call(
      *const_cast<ton_api::adnl_Address *>(addr.get()),
      td::overloaded([&](const ton_api::adnl_address_udp &obj) { res = td::make_ref<AdnlAddressUdp>(obj); },
                     [&](const ton_api::adnl_address_udp6 &obj) { res = td::make_ref<AdnlAddressUdp6>(obj); },
                     [&](const ton_api::adnl_address_tunnel &obj) { res = td::make_ref<AdnlAddressTunnel>(obj); }));
  return res;
}

bool AdnlAddressList::public_only() const {
  for (auto &addr : addrs_) {
    if (!addr->is_public()) {
      return false;
    }
  }
  return true;
}

AdnlAddressList::AdnlAddressList(const tl_object_ptr<ton_api::adnl_addressList> &addrs) {
  version_ = static_cast<td::uint32>(addrs->version_);
  std::vector<td::Ref<AdnlAddressImpl>> vec;
  for (auto &addr : addrs->addrs_) {
    vec.push_back(AdnlAddressImpl::create(addr));
  }
  addrs_ = std::move(vec);
  reinit_date_ = addrs->reinit_date_;
  priority_ = addrs->priority_;
  expire_at_ = addrs->expire_at_;
}

tl_object_ptr<ton_api::adnl_addressList> AdnlAddressList::tl() const {
  std::vector<tl_object_ptr<ton_api::adnl_Address>> addrs;
  for (auto &v : addrs_) {
    addrs.emplace_back(v->tl());
  }
  return create_tl_object<ton_api::adnl_addressList>(std::move(addrs), version_, reinit_date_, priority_, expire_at_);
}

td::uint32 AdnlAddressList::serialized_size() const {
  td::uint32 res = 24;
  for (auto &addr : addrs_) {
    res += addr->serialized_size();
  }
  return res;
}

td::Result<AdnlAddressList> AdnlAddressList::create(const tl_object_ptr<ton_api::adnl_addressList> &addr_list) {
  auto A = AdnlAddressList{addr_list};
  if (A.serialized_size() > max_serialized_size()) {
    return td::Status::Error(ErrorCode::protoviolation, PSTRING() << "too big addr list: size=" << A.serialized_size());
  }
  return A;
}

td::Status AdnlAddressList::add_udp_address(td::IPAddress addr) {
  if (addr.is_ipv4()) {
    auto r = td::make_ref<AdnlAddressUdp>(addr.get_ipv4(), static_cast<td::uint16>(addr.get_port()));
    addrs_.push_back(std::move(r));
    return td::Status::OK();
  } else {
    return td::Status::Error(ErrorCode::protoviolation, "only works with ipv4");
  }
}

}  // namespace adnl

}  // namespace ton
