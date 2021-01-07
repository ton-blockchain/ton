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

#include "adnl.h"
#include "adnl-peer-table.h"
#include "keys/encryptor.h"

#include <map>

namespace ton {

namespace adnl {

class AdnlInboundTunnelPoint : public AdnlTunnel {
 public:
  virtual ~AdnlInboundTunnelPoint() = default;
  virtual void receive_packet(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice datagram) = 0;
};

class AdnlInboundTunnelEndpoint : public AdnlInboundTunnelPoint {
 public:
  AdnlInboundTunnelEndpoint(PublicKeyHash pubkey_hash, std::vector<PublicKeyHash> decrypt_via, AdnlNodeIdShort proxy_to,
                            td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<AdnlPeerTable> adnl)
      : pubkey_hash_(std::move(pubkey_hash))
      , decrypt_via_(std::move(decrypt_via))
      , proxy_to_(std::move(proxy_to))
      , keyring_(std::move(keyring))
      , adnl_(std::move(adnl)) {
  }

  void receive_packet(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice datagram) override;
  void receive_packet_cont(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice datagram, size_t idx);
  void decrypted_packet(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice data, size_t idx);

 private:
  PublicKeyHash pubkey_hash_;
  std::vector<PublicKeyHash> decrypt_via_;
  AdnlNodeIdShort proxy_to_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<AdnlPeerTable> adnl_;
};

class AdnlInboundTunnelMidpoint : public AdnlInboundTunnelPoint {
 public:
  AdnlInboundTunnelMidpoint(ton::PublicKey encrypt_via, AdnlNodeIdShort proxy_to, AdnlNodeIdShort proxy_as,
                            td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<AdnlPeerTable> adnl)
      : encrypt_via_(std::move(encrypt_via)), proxy_to_(proxy_to), proxy_as_(proxy_as), keyring_(keyring), adnl_(adnl) {
  }
  void start_up() override;
  void receive_packet(AdnlNodeIdShort src, td::IPAddress src_addr, td::BufferSlice datagram) override;

 private:
  ton::PublicKeyHash encrypt_key_hash_;
  ton::PublicKey encrypt_via_;
  std::unique_ptr<Encryptor> encryptor_;
  AdnlNodeIdShort proxy_to_;
  AdnlNodeIdShort proxy_as_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<AdnlPeerTable> adnl_;
};

class AdnlProxyNode : public td::actor::Actor {
 public:
  void receive_message(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data);
  void receive_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise);

 private:
  std::map<PublicKeyHash, td::actor::ActorOwn<AdnlInboundTunnelMidpoint>> mid_;
};

}  // namespace adnl

}  // namespace ton
