#include <cstring>

#include "td/actor/actor.h"
#include "td/utils/Random.h"

#include "quic-pimpl.h"
#include "quic-server.h"

namespace ton::quic {

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::create(int port, td::Ed25519::PrivateKey server_key,
                                                               std::unique_ptr<Callback> callback, td::Slice alpn,
                                                               td::Slice bind_host) {
  td::IPAddress local_addr;
  std::string bind_host_str = bind_host.str();
  TRY_STATUS(local_addr.init_host_port(td::CSlice(bind_host_str.c_str()), port));

  TRY_RESULT(fd, td::UdpSocketFd::open(local_addr));

  auto name = PSTRING() << "QUIC:" << local_addr;
  return td::actor::create_actor<QuicServer>(td::actor::ActorOptions().with_name(name).with_poll(true), std::move(fd),
                                             std::move(server_key), td::BufferSlice(alpn), std::move(callback));
}

QuicServer::QuicServer(td::UdpSocketFd fd, td::Ed25519::PrivateKey server_key, td::BufferSlice alpn,
                       std::unique_ptr<Callback> callback)
    : fd_(std::move(fd)), alpn_(std::move(alpn)), server_key_(std::move(server_key)), callback_(std::move(callback)) {
}

void QuicServer::start_up() {
  LOG(INFO) << "starting up";
  self_id_ = actor_id(this);
  td::actor::SchedulerContext::get().get_poll().subscribe(fd_.get_poll_info().extract_pollable_fd(this),
                                                          td::PollFlags::ReadWrite());
  LOG(INFO) << "startup completed";
}

void QuicServer::tear_down() {
  td::actor::SchedulerContext::get().get_poll().unsubscribe(fd_.get_poll_info().get_pollable_fd_ref());
  LOG(INFO) << "tear down";
}

void QuicServer::hangup() {
  LOG(ERROR) << "unexpected hangup signal";
}

void QuicServer::hangup_shared() {
  LOG(ERROR) << "unexpected hangup_shared signal";
}

void QuicServer::wake_up() {
  LOG(ERROR) << "unexpected wake_up signal";
}

QuicServer::ConnectionState *QuicServer::find_connection(const QuicConnectionId &cid) {
  auto it = connections_.find(cid);
  if (it != connections_.end()) {
    return it->second.get();
  }
  return nullptr;
}

void QuicServer::remove_connection(const QuicConnectionId &cid) {
  connections_.erase(cid);
}

void QuicServer::alarm() {
  for (auto it = connections_.begin(); it != connections_.end();) {
    auto &cid = it->first;
    auto &state = it->second;

    if (!state->p_impl) {
      it = connections_.erase(it);
      continue;
    }

    if (state->p_impl->is_expired()) {
      auto R = state->p_impl->handle_expiry();
      if (R.is_error()) {
        it = connections_.erase(it);
        continue;
      }

      switch (R.ok()) {
        case QuicConnectionPImpl::ExpiryAction::None:
          break;
        case QuicConnectionPImpl::ExpiryAction::ScheduleWrite:
          flush_egress_for(cid, *state);
          break;
        case QuicConnectionPImpl::ExpiryAction::IdleClose:
          it = connections_.erase(it);
          continue;
        case QuicConnectionPImpl::ExpiryAction::Close:
          flush_egress_for(cid, *state);
          it = connections_.erase(it);
          continue;
      }
    }

    ++it;
  }
  update_alarm();
}

void QuicServer::loop() {
  LOG(ERROR) << "unexpected loop signal";
}

void QuicServer::notify() {
  td::actor::send_closure(self_id_, &QuicServer::on_fd_notify);
}

void QuicServer::on_fd_notify() {
  td::sync_with_poll(fd_);
  drain_ingress();
  flush_egress_all();
  update_alarm();
}

class QuicServer::PImplCallback final : public QuicConnectionPImpl::Callback {
 public:
  explicit PImplCallback(QuicServer &server) : server_(server) {
  }

  void set_connection_id(QuicConnectionId cid) override {
    cid_ = cid;
  }

  td::Status on_handshake_completed(HandshakeCompletedEvent event) override {
    if (server_.callback_) {
      return server_.callback_->on_connected(cid_, std::move(event.peer_public_key));
    }
    return td::Status::OK();
  }

  void on_stream_data(StreamDataEvent event) override {
    if (server_.callback_) {
      server_.callback_->on_stream_data(cid_, event.sid, std::move(event.data));
      if (event.fin) {
        server_.callback_->on_stream_end(cid_, event.sid);
      }
    }
  }

 private:
  QuicServer &server_;
  QuicConnectionId cid_;
};

td::Result<std::shared_ptr<QuicServer::ConnectionState>> QuicServer::get_or_create_connection(
    const UdpMessageBuffer &msg_in) {
  TRY_RESULT(vc, VersionCid::from_datagram(td::Slice(msg_in.storage)));

  // Try lookup by DCID (works for post-handshake short-header packets using server's SCID)
  auto it = connections_.find(vc.dcid);
  if (it != connections_.end()) {
    return it->second;
  }

  // Try lookup by source CID from packet (works for handshake long-header packets with client SCID)
  QuicConnectionId client_scid = vc.scid;
  if (!client_scid.empty()) {
    if (it = connections_.find(client_scid); it != connections_.end()) {
      return it->second;
    }
  }

  TRY_RESULT(local_address, fd_.get_local_address());

  TRY_RESULT(p_impl, QuicConnectionPImpl::create_server(local_address, msg_in.address, server_key_, alpn_.as_slice(),
                                                        vc, std::make_unique<PImplCallback>(*this)));

  QuicConnectionId server_scid = p_impl->get_primary_scid();

  LOG(INFO) << "creating new inbound connection from " << msg_in.address << " server_scid=" << server_scid
            << " client_scid=" << client_scid;

  auto state = std::make_shared<ConnectionState>();
  state->p_impl = std::move(p_impl);
  state->remote_address = msg_in.address;
  state->is_outbound = false;

  // Store by BOTH server SCID and client SCID to support both handshake and post-handshake lookup
  connections_[server_scid] = state;
  if (!client_scid.empty()) {
    connections_[client_scid] = state;
  }

  return state;
}

td::Result<QuicConnectionId> QuicServer::connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key,
                                                 td::Slice alpn) {
  td::IPAddress remote_address;
  std::string host_str(host.begin(), host.end());
  TRY_STATUS(remote_address.init_host_port(td::CSlice(host_str.c_str()), port));

  TRY_RESULT(local_address, fd_.get_local_address());

  TRY_RESULT(p_impl, QuicConnectionPImpl::create_client(local_address, remote_address, std::move(client_key), alpn,
                                                        std::make_unique<PImplCallback>(*this)));
  QuicConnectionId cid = p_impl->get_primary_scid();

  LOG(INFO) << "creating outbound connection to " << remote_address << " cid=" << cid;

  auto state = std::make_shared<ConnectionState>();
  state->p_impl = std::move(p_impl);
  state->remote_address = remote_address;
  state->is_outbound = true;

  connections_[cid] = state;

  flush_egress_for(cid, *state);
  update_alarm();

  return QuicConnectionId(cid);
}

void QuicServer::drain_ingress() {
  bool run = true;
  auto cycle = [this, &run]() -> td::Status {
    char buf[DEFAULT_MTU];
    td::MutableSlice slice(buf, buf + DEFAULT_MTU);
    UdpMessageBuffer msg_in;
    td::UdpSocketFd::InboundMessage msg{.from = &msg_in.address, .data = slice, .error = nullptr};
    TRY_STATUS(fd_.receive_message(msg, run));

    if (!run) {
      return td::Status::OK();
    }

    msg_in.storage = msg.data;

    auto R = get_or_create_connection(msg_in);
    if (R.is_error()) {
      LOG(WARNING) << "dropping inbound packet from " << msg_in.address << ": " << R.error();
      return td::Status::OK();
    }
    auto state = R.move_as_ok();

    if (auto status = state->p_impl->handle_ingress(msg_in); status.is_error()) {
      LOG(WARNING) << "failed to handle ingress from " << state->remote_address << " ("
                   << (state->is_outbound ? "outbound" : "inbound") << "): " << status;
      // Note: don't call remove_connection here - the connection might be stored under multiple CIDs
      return td::Status::OK();
    }

    auto cid = state->p_impl->get_primary_scid();
    flush_egress_for(cid, *state);
    return td::Status::OK();
  };

  while (run) {
    if (auto status = cycle(); status.is_error()) {
      LOG(ERROR) << "failed to drain incoming traffic: " << status;
      break;
    }
  }
}

void QuicServer::flush_egress_for(QuicConnectionId cid, ConnectionState &state, EgressData data) {
  if (!state.p_impl) {
    return;
  }

  if (data.stream_data.has_value()) {
    auto &stream_data = data.stream_data.value();
    auto cycle = [this, &state, &stream_data]() -> td::Status {
      char buf[DEFAULT_MTU];
      UdpMessageBuffer msg_out;
      msg_out.storage = td::MutableSlice(buf, DEFAULT_MTU);
      TRY_STATUS(state.p_impl->write_stream(msg_out, stream_data.sid, std::move(stream_data.data), stream_data.fin));

      if (msg_out.storage.empty()) {
        return td::Status::OK();
      }

      bool sent = false;
      td::UdpSocketFd::OutboundMessage msg{.to = &msg_out.address, .data = td::Slice{msg_out.storage}};
      TRY_STATUS(fd_.send_message(msg, sent));
      if (!sent) {
        LOG(WARNING) << "outbound message lost to " << msg_out.address;
      }
      return td::Status::OK();
    };

    if (auto status = cycle(); status.is_error()) {
      LOG(WARNING) << "failed to send stream data: " << status;
      return;
    }
  }

  bool run = true;
  auto cycle = [this, &state, &run]() -> td::Status {
    char buf[DEFAULT_MTU];
    UdpMessageBuffer msg_out;
    msg_out.storage = td::MutableSlice(buf, DEFAULT_MTU);
    TRY_STATUS(state.p_impl->produce_egress(msg_out));
    run = !msg_out.storage.empty();

    if (!run) {
      return td::Status::OK();
    }

    td::UdpSocketFd::OutboundMessage msg{.to = &msg_out.address, .data = td::Slice{msg_out.storage}};
    TRY_STATUS(fd_.send_message(msg, run));
    if (!run) {
      LOG(WARNING) << "outbound message lost to " << msg_out.address;
    }
    return td::Status::OK();
  };

  while (run) {
    if (auto status = cycle(); status.is_error()) {
      LOG(WARNING) << "failed to flush egress: " << status;
      break;
    }
  }
}

void QuicServer::flush_egress_all() {
  for (auto &it : connections_) {
    flush_egress_for(it.first, *it.second);
  }
}

void QuicServer::update_alarm() {
  td::Timestamp alarm_ts = td::Timestamp::never();
  for (auto &it : connections_) {
    it.second->p_impl->relax_alarm_timestamp(alarm_ts);
  }
  alarm_timestamp() = alarm_ts;
}

void QuicServer::send_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data) {
  auto *state = find_connection(cid);
  if (!state) {
    LOG(WARNING) << "send_stream_data to unknown connection cid=" << cid;
    return;
  }
  flush_egress_for(cid, *state,
                   {.stream_data = EgressData::StreamData{.sid = sid, .data = std::move(data), .fin = false}});
  update_alarm();
}

void QuicServer::send_stream_end(QuicConnectionId cid, QuicStreamID sid) {
  auto *state = find_connection(cid);
  if (!state) {
    LOG(WARNING) << "send_stream_end to unknown connection cid=" << cid;
    return;
  }
  flush_egress_for(cid, *state, {.stream_data = EgressData::StreamData{.sid = sid, .data = {}, .fin = true}});
  update_alarm();
}

td::Result<QuicStreamID> QuicServer::open_stream(QuicConnectionId cid) {
  auto *state = find_connection(cid);
  if (!state || !state->p_impl) {
    return td::Status::Error("Connection not found");
  }
  return state->p_impl->open_stream();
}

}  // namespace ton::quic
