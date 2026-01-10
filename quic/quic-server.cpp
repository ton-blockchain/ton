#include <cstring>

#include "td/actor/actor.h"

#include "quic-pimpl.h"
#include "quic-server.h"

namespace ton::quic {

namespace {

td::Result<ngtcp2_version_cid> decode_version_cid(td::Slice datagram) {
  if (datagram.size() < 7) {
    return td::Status::Error("QUIC packet too small");
  }
  const auto *p = reinterpret_cast<const uint8_t *>(datagram.data());
  const auto n = datagram.size();

  if ((p[0] & 0x80u) == 0) {
    return td::Status::Error("not a long header QUIC packet");
  }

  uint32_t version = (static_cast<uint32_t>(p[1]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
                     (static_cast<uint32_t>(p[3]) << 8) | static_cast<uint32_t>(p[4]);
  size_t off = 5;

  uint8_t dcid_len = p[off++];
  if (off + dcid_len + 1 > n) {
    return td::Status::Error("invalid DCID length");
  }
  const uint8_t *dcid = p + off;
  off += dcid_len;

  uint8_t scid_len = p[off++];
  if (off + scid_len > n) {
    return td::Status::Error("invalid SCID length");
  }
  const uint8_t *scid = p + off;

  ngtcp2_version_cid vc{};
  vc.version = version;
  vc.dcid = dcid;
  vc.dcidlen = dcid_len;
  vc.scid = scid;
  vc.scidlen = scid_len;
  return vc;
}

}  // namespace

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::listen(int port, td::Slice cert_file, td::Slice key_file,
                                                               std::unique_ptr<Callback> callback, td::Slice alpn,
                                                               td::Slice bind_host) {
  td::IPAddress local_addr;
  std::string bind_host_str = bind_host.str();
  TRY_STATUS(local_addr.init_host_port(td::CSlice(bind_host_str.c_str()), port));

  TRY_RESULT(fd, td::UdpSocketFd::open(local_addr));

  auto name = PSTRING() << "QUIC:" << local_addr;
  return td::actor::create_actor<QuicServer>(td::actor::ActorOptions().with_name(name).with_poll(true), std::move(fd),
                                             td::BufferSlice(cert_file), td::BufferSlice(key_file),
                                             td::BufferSlice(alpn), std::move(callback));
}

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::listen_rpk(int port, td::Ed25519::PrivateKey server_key,
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

QuicServer::QuicServer(td::UdpSocketFd fd, td::BufferSlice cert_file, td::BufferSlice key_file, td::BufferSlice alpn,
                       std::unique_ptr<Callback> callback)
    : fd_(std::move(fd))
    , cert_file_(std::move(cert_file))
    , key_file_(std::move(key_file))
    , alpn_(std::move(alpn))
    , callback_(std::move(callback)) {
}

QuicServer::QuicServer(td::UdpSocketFd fd, td::Ed25519::PrivateKey server_key, td::BufferSlice alpn,
                       std::unique_ptr<Callback> callback)
    : fd_(std::move(fd))
    , alpn_(std::move(alpn))
    , server_key_(std::move(server_key))
    , use_rpk_(true)
    , callback_(std::move(callback)) {
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

void QuicServer::alarm() {
  for (auto it = connections_.begin(); it != connections_.end();) {
    auto &peer = it->first;
    auto &state = it->second;

    if (!state.p_impl) {
      it = connections_.erase(it);
      continue;
    }

    if (state.p_impl->is_expired()) {
      auto R = state.p_impl->handle_expiry();
      if (R.is_error()) {
        it = connections_.erase(it);
        continue;
      }

      switch (R.ok()) {
        case QuicConnectionPImpl::ExpiryAction::None:
          break;
        case QuicConnectionPImpl::ExpiryAction::ScheduleWrite:
          flush_egress_for(peer, state);
          break;
        case QuicConnectionPImpl::ExpiryAction::IdleClose:
          it = connections_.erase(it);
          continue;
        case QuicConnectionPImpl::ExpiryAction::Close:
          flush_egress_for(peer, state);
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

td::Result<QuicServer::ConnectionState *> QuicServer::get_or_create_connection(const UdpMessageBuffer &msg_in) {
  auto it = connections_.find(msg_in.address);
  if (it != connections_.end()) {
    return &it->second;
  }

  TRY_RESULT(vc, decode_version_cid(td::Slice(msg_in.storage)));

  ConnectionState state;
  state.p_impl = std::make_unique<QuicConnectionPImpl>();
  state.p_impl->remote_address = msg_in.address;
  TRY_RESULT_ASSIGN(state.p_impl->local_address, fd_.get_local_address());

  if (use_rpk_) {
    TRY_STATUS(state.p_impl->init_tls_server_rpk(*server_key_, alpn_.as_slice()));
  } else {
    TRY_STATUS(state.p_impl->init_tls_server(cert_file_.as_slice(), key_file_.as_slice(), alpn_.as_slice()));
  }
  TRY_STATUS(state.p_impl->init_quic_server(vc));

  class PImplCallback final : public QuicConnectionPImpl::Callback {
   public:
    PImplCallback(QuicServer &server, td::IPAddress peer) : server_(server), peer_(peer) {
    }

    td::Status on_handshake_completed(HandshakeCompletedEvent event) override {
      if (server_.callback_) {
        server_.callback_->on_connected(peer_, std::move(event.peer_public_key));
      }
      return td::Status::OK();
    }

    void on_stream_data(StreamDataEvent event) override {
      if (server_.callback_) {
        server_.callback_->on_stream_data(peer_, event.sid, std::move(event.data));
        if (event.fin) {
          server_.callback_->on_stream_end(peer_, event.sid);
        }
      }
    }

   private:
    QuicServer &server_;
    td::IPAddress peer_;
  };

  state.p_impl->callback = std::make_unique<PImplCallback>(*this, msg_in.address);

  auto ins = connections_.emplace(msg_in.address, std::move(state));
  return &ins.first->second;
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
    auto *state = R.ok();

    if (auto status = state->p_impl->handle_ingress(msg_in); status.is_error()) {
      LOG(WARNING) << "failed to handle ingress from " << msg_in.address << ": " << status;
      connections_.erase(msg_in.address);
      return td::Status::OK();
    }

    flush_egress_for(msg_in.address, *state);
    return td::Status::OK();
  };

  while (run) {
    if (auto status = cycle(); status.is_error()) {
      LOG(ERROR) << "failed to drain incoming traffic: " << status;
      break;
    }
  }
}

void QuicServer::flush_egress_for(const td::IPAddress &peer, ConnectionState &state, EgressData data) {
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
      LOG(WARNING) << "failed to send stream data to " << peer << ": " << status;
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
      LOG(WARNING) << "failed to flush egress to " << peer << ": " << status;
      break;
    }
  }
}

void QuicServer::flush_egress_all() {
  for (auto &it : connections_) {
    flush_egress_for(it.first, it.second);
  }
}

void QuicServer::update_alarm() {
  td::Timestamp alarm_ts = td::Timestamp::never();
  for (auto &it : connections_) {
    if (it.second.p_impl) {
      it.second.p_impl->relax_alarm_timestamp(alarm_ts);
    }
  }
  alarm_timestamp() = alarm_ts;
}

void QuicServer::send_stream_data(const td::IPAddress &peer, QuicStreamID sid, td::BufferSlice data) {
  auto it = connections_.find(peer);
  if (it == connections_.end()) {
    LOG(WARNING) << "send_stream_data to unknown peer " << peer;
    return;
  }
  flush_egress_for(peer, it->second,
                   {.stream_data = EgressData::StreamData{.sid = sid, .data = std::move(data), .fin = false}});
  update_alarm();
}

void QuicServer::send_stream_end(const td::IPAddress &peer, QuicStreamID sid) {
  auto it = connections_.find(peer);
  if (it == connections_.end()) {
    LOG(WARNING) << "send_stream_end to unknown peer " << peer;
    return;
  }
  flush_egress_for(peer, it->second, {.stream_data = EgressData::StreamData{.sid = sid, .data = {}, .fin = true}});
  update_alarm();
}

}  // namespace ton::quic
