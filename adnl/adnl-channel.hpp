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

#include "adnl-channel.h"
#include "keys/encryptor.h"

namespace ton {

namespace adnl {

class AdnlPeerPair;

class AdnlChannelImpl : public AdnlChannel {
 public:
  AdnlChannelImpl(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id, td::actor::ActorId<AdnlPeerPair> peer_pair,
                  AdnlChannelIdShort in_id, AdnlChannelIdShort out_id, std::unique_ptr<Encryptor> encryptor,
                  std::unique_ptr<Decryptor> decryptor);
  void decrypt(td::BufferSlice data, td::Promise<AdnlPacket> promise);
  void receive(td::IPAddress addr, td::BufferSlice data) override;
  void send_message(td::uint32 priority, td::actor::ActorId<AdnlNetworkConnection> conn, td::BufferSlice data) override;

  struct AdnlChannelPrintId {
    AdnlChannelIdShort channel_out_id_;
    AdnlChannelIdShort channel_in_id_;
    AdnlNodeIdShort local_id_;
    AdnlNodeIdShort peer_id_;
  };

  AdnlChannelPrintId print_id() const {
    return AdnlChannelPrintId{channel_out_id_, channel_in_id_, local_id_, peer_id_};
  }

 private:
  AdnlChannelIdShort channel_out_id_;
  AdnlChannelIdShort channel_in_id_;
  AdnlNodeIdShort local_id_;
  AdnlNodeIdShort peer_id_;
  std::unique_ptr<Encryptor> encryptor_;
  std::unique_ptr<Decryptor> decryptor_;
  td::actor::ActorId<AdnlPeerPair> peer_pair_;
};

}  // namespace adnl

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::adnl::AdnlChannelImpl::AdnlChannelPrintId &id) {
  sb << "[channel " << id.peer_id_ << "-" << id.local_id_ << " " << id.channel_out_id_ << "-" << id.channel_in_id_
     << "]";
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::adnl::AdnlChannelImpl &channel) {
  sb << channel.print_id();
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::adnl::AdnlChannelImpl *channel) {
  sb << channel->print_id();
  return sb;
}

}  // namespace td
