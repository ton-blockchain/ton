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
#include "adnl-channel.hpp"
#include "adnl-peer.h"
#include "adnl-peer-table.h"

#include "td/utils/crypto.h"
#include "crypto/Ed25519.h"

namespace ton {

namespace adnl {

td::Result<td::actor::ActorOwn<AdnlChannel>> AdnlChannel::create(privkeys::Ed25519 pk_data, pubkeys::Ed25519 pub_data,
                                                                 AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id,
                                                                 AdnlChannelIdShort &out_id, AdnlChannelIdShort &in_id,
                                                                 td::actor::ActorId<AdnlPeerPair> peer_pair) {
  td::Ed25519::PublicKey pub_k = pub_data.export_key();
  td::Ed25519::PrivateKey priv_k = pk_data.export_key();

  TRY_RESULT_PREFIX(shared_secret, td::Ed25519::compute_shared_secret(pub_k, priv_k),
                    "failed to compute channel shared secret: ");
  CHECK(shared_secret.length() == 32);

  td::SecureString rev_secret{32};
  for (td::uint32 i = 0; i < 32; i++) {
    rev_secret.as_mutable_slice()[i] = shared_secret[31 - i];
  }

  auto R = [&]() -> std::pair<PrivateKey, PublicKey> {
    if (local_id < peer_id) {
      return {privkeys::AES{std::move(shared_secret)}, pubkeys::AES{std::move(rev_secret)}};
    } else if (peer_id < local_id) {
      return {privkeys::AES{std::move(rev_secret)}, pubkeys::AES{std::move(shared_secret)}};
    } else {
      auto c = shared_secret.copy();
      return {privkeys::AES{std::move(c)}, pubkeys::AES{std::move(shared_secret)}};
    }
  }();

  in_id = AdnlChannelIdShort{R.first.compute_short_id()};
  out_id = AdnlChannelIdShort{R.second.compute_short_id()};

  TRY_RESULT_PREFIX(encryptor, R.second.create_encryptor(), "failed to init channel encryptor: ");
  TRY_RESULT_PREFIX(decryptor, R.first.create_decryptor(), "failed to init channel decryptor: ");

  return td::actor::create_actor<AdnlChannelImpl>("channel", local_id, peer_id, peer_pair, in_id, out_id,
                                                  std::move(encryptor), std::move(decryptor));
}

AdnlChannelImpl::AdnlChannelImpl(AdnlNodeIdShort local_id, AdnlNodeIdShort peer_id,
                                 td::actor::ActorId<AdnlPeerPair> peer_pair, AdnlChannelIdShort in_id,
                                 AdnlChannelIdShort out_id, std::unique_ptr<Encryptor> encryptor,
                                 std::unique_ptr<Decryptor> decryptor) {
  local_id_ = local_id;
  peer_id_ = peer_id;

  encryptor_ = std::move(encryptor);
  decryptor_ = std::move(decryptor);

  channel_in_id_ = in_id;
  channel_out_id_ = out_id;

  peer_pair_ = peer_pair;

  VLOG(ADNL_INFO) << this << ": created";
}

void AdnlChannelImpl::decrypt(td::BufferSlice raw_data, td::Promise<AdnlPacket> promise) {
  TRY_RESULT_PROMISE_PREFIX(promise, data, decryptor_->decrypt(raw_data.as_slice()),
                            "failed to decrypt channel message: ");
  TRY_RESULT_PROMISE_PREFIX(promise, tl_packet, fetch_tl_object<ton_api::adnl_packetContents>(std::move(data), true),
                            "decrypted channel packet contains invalid TL scheme: ");
  TRY_RESULT_PROMISE_PREFIX(promise, packet, AdnlPacket::create(std::move(tl_packet)), "received bad packet: ");
  if (packet.inited_from_short() && packet.from_short() != peer_id_) {
    promise.set_error(td::Status::Error(ErrorCode::protoviolation, "bad channel packet destination"));
    return;
  }
  promise.set_value(std::move(packet));
}

void AdnlChannelImpl::send_message(td::uint32 priority, td::actor::ActorId<AdnlNetworkConnection> conn,
                                   td::BufferSlice data) {
  auto E = encryptor_->encrypt(data.as_slice());
  if (E.is_error()) {
    VLOG(ADNL_ERROR) << this << ": dropping OUT message: can not encrypt: " << E.move_as_error();
    return;
  }
  auto enc = E.move_as_ok();
  auto B = td::BufferSlice(enc.size() + 32);
  td::MutableSlice S = B.as_slice();
  S.copy_from(channel_out_id_.as_slice());
  S.remove_prefix(32);
  S.copy_from(enc.as_slice());
  td::actor::send_closure(conn, &AdnlNetworkConnection::send, local_id_, peer_id_, priority, std::move(B));
}

void AdnlChannelImpl::receive(td::IPAddress addr, td::BufferSlice data) {
  auto P = td::PromiseCreator::lambda(
      [peer = peer_pair_, channel_id = channel_in_id_, addr, id = print_id()](td::Result<AdnlPacket> R) {
        if (R.is_error()) {
          VLOG(ADNL_WARNING) << id << ": dropping IN message: can not decrypt: " << R.move_as_error();
        } else {
          auto packet = R.move_as_ok();
          packet.set_remote_addr(addr);
          td::actor::send_closure(peer, &AdnlPeerPair::receive_packet_from_channel, channel_id, std::move(packet));
        }
      });

  decrypt(std::move(data), std::move(P));
}

}  // namespace adnl

}  // namespace ton
