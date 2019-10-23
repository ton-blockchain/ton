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
#include "adnl-ext-server.hpp"
#include "keys/encryptor.h"
#include "utils.hpp"

namespace ton {

namespace adnl {

td::Status AdnlInboundConnection::process_packet(td::BufferSlice data) {
  TRY_RESULT(f, fetch_tl_object<ton_api::adnl_message_query>(std::move(data), true));

  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this), query_id = f->query_id_](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          auto S = R.move_as_error();
          LOG(WARNING) << "failed ext query: " << S;
        } else {
          auto B = create_tl_object<ton_api::adnl_message_answer>(query_id, R.move_as_ok());
          td::actor::send_closure(SelfId, &AdnlInboundConnection::send, serialize_tl_object(B, true));
        }
      });
  td::actor::send_closure(peer_table_, &AdnlPeerTable::deliver_query, remote_id_, local_id_, std::move(f->query_),
                          std::move(P));
  return td::Status::OK();
}

td::Status AdnlInboundConnection::process_init_packet(td::BufferSlice data) {
  if (data.size() < 32) {
    return td::Status::Error(ErrorCode::protoviolation, "too small init packet");
  }
  local_id_ = AdnlNodeIdShort{data.as_slice().truncate(32)};
  data.confirm_read(32);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    td::actor::send_closure(SelfId, &AdnlInboundConnection::inited_crypto, std::move(R));
  });

  td::actor::send_closure(ext_server_, &AdnlExtServerImpl::decrypt_init_packet, local_id_, std::move(data),
                          std::move(P));
  stop_read();
  return td::Status::OK();
}

void AdnlInboundConnection::inited_crypto(td::Result<td::BufferSlice> R) {
  if (R.is_error()) {
    LOG(ERROR) << "failed to init crypto: " << R.move_as_error();
    stop();
    return;
  }
  auto S = init_crypto(R.move_as_ok().as_slice());
  if (S.is_error()) {
    LOG(ERROR) << "failed to init crypto (2): " << R.move_as_error();
    stop();
    return;
  }
  send(td::BufferSlice());
  resume_read();
  notify();
}

td::Status AdnlInboundConnection::process_custom_packet(td::BufferSlice &data, bool &processed) {
  if (data.size() == 12) {
    auto F = fetch_tl_object<ton_api::tcp_ping>(data.clone(), true);
    if (F.is_ok()) {
      auto f = F.move_as_ok();
      auto obj = create_tl_object<ton_api::tcp_pong>(f->random_id_);
      send(serialize_tl_object(obj, true));
      processed = true;
      return td::Status::OK();
    }
  }
  if (1) {
    auto F = fetch_tl_object<ton_api::tcp_authentificate>(data.clone(), true);
    if (F.is_ok()) {
      if (nonce_.size() > 0 || !remote_id_.is_zero()) {
        return td::Status::Error(ErrorCode::protoviolation, "duplicate authentificate");
      }
      auto f = F.move_as_ok();
      nonce_ = td::SecureString{f->nonce_.size() + 256};
      nonce_.as_mutable_slice().truncate(f->nonce_.size()).copy_from(f->nonce_.as_slice());
      td::Random::secure_bytes(nonce_.as_mutable_slice().remove_prefix(f->nonce_.size()));

      auto obj = create_tl_object<ton_api::tcp_authentificationNonce>(
          td::BufferSlice{nonce_.as_slice().remove_prefix(f->nonce_.size())});
      send(serialize_tl_object(obj, true));
      processed = true;
      return td::Status::OK();
    }
  }

  if (nonce_.size() != 0) {
    auto F = fetch_tl_object<ton_api::tcp_authentificationComplete>(data.clone(), true);
    if (F.is_ok()) {
      auto f = F.move_as_ok();
      if (nonce_.size() == 0 || !remote_id_.is_zero()) {
        return td::Status::Error(ErrorCode::protoviolation, "duplicate authentificate");
      }

      auto pub_key = PublicKey{f->key_};
      TRY_RESULT(enc, pub_key.create_encryptor());
      TRY_STATUS(enc->check_signature(nonce_.as_slice(), f->signature_.as_slice()));

      remote_id_ = AdnlNodeIdShort{pub_key.compute_short_id()};
      nonce_.clear();
      processed = true;
      return td::Status::OK();
    }
  }

  return td::Status::OK();
}

void AdnlExtServerImpl::add_tcp_port(td::uint16 port) {
  auto it = listeners_.find(port);
  if (it != listeners_.end()) {
    return;
  }

  class Callback : public td::TcpListener::Callback {
   private:
    td::actor::ActorId<AdnlExtServerImpl> id_;

   public:
    Callback(td::actor::ActorId<AdnlExtServerImpl> id) : id_(id) {
    }
    void accept(td::SocketFd fd) override {
      td::actor::send_closure(id_, &AdnlExtServerImpl::accepted, std::move(fd));
    }
  };

  auto act = td::actor::create_actor<td::TcpInfiniteListener>(
      td::actor::ActorOptions().with_name("listener").with_poll(), port, std::make_unique<Callback>(actor_id(this)));
  listeners_.emplace(port, std::move(act));
}

void AdnlExtServerImpl::add_local_id(AdnlNodeIdShort id) {
  local_ids_.insert(id);
}

void AdnlExtServerImpl::accepted(td::SocketFd fd) {
  td::actor::create_actor<AdnlInboundConnection>(td::actor::ActorOptions().with_name("inconn").with_poll(),
                                                 std::move(fd), peer_table_, actor_id(this))
      .release();
}

void AdnlExtServerImpl::decrypt_init_packet(AdnlNodeIdShort dst, td::BufferSlice data,
                                            td::Promise<td::BufferSlice> promise) {
  auto it = local_ids_.find(dst);
  if (it != local_ids_.end()) {
    td::actor::send_closure(peer_table_, &AdnlPeerTable::decrypt_message, dst, std::move(data), std::move(promise));
  } else {
    promise.set_error(td::Status::Error());
  }
}

td::actor::ActorOwn<AdnlExtServer> AdnlExtServerCreator::create(td::actor::ActorId<AdnlPeerTable> adnl,
                                                                std::vector<AdnlNodeIdShort> ids,
                                                                std::vector<td::uint16> ports) {
  return td::actor::create_actor<AdnlExtServerImpl>("extserver", adnl, std::move(ids), std::move(ports));
}

}  // namespace adnl

}  // namespace ton
