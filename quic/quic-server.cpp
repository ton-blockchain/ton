#include <cstring>

#include "td/actor/actor.h"
#include "td/utils/Random.h"

#include "quic-pimpl.h"
#include "quic-server.h"

namespace ton::quic {

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::create(int port, td::Ed25519::PrivateKey server_key,
                                                               std::unique_ptr<Callback> callback, td::Slice alpn,
                                                               td::Slice bind_host) {
  CHECK(callback);
  td::IPAddress local_addr;
  TRY_STATUS(local_addr.init_host_port(bind_host.str(), port));

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
  stop();
}

void QuicServer::hangup_shared() {
  LOG(ERROR) << "unexpected hangup_shared signal";
}

std::shared_ptr<QuicServer::ConnectionState> QuicServer::find_connection(const QuicConnectionId &cid) {
  if (auto it = connections_.find(cid); it != connections_.end()) {
    return it->second;
  }
  return nullptr;
}

bool QuicServer::try_close(ConnectionState &state) {
  if (!state.impl().is_expired()) {
    return false;
  }
  auto R = state.impl().handle_expiry();
  if (R.is_error()) {
    return true;
  }

  switch (R.ok()) {
    case QuicConnectionPImpl::ExpiryAction::None:
      LOG(DEBUG) << "expiry None for " << state.remote_address;
      return false;
    case QuicConnectionPImpl::ExpiryAction::ScheduleWrite:
      LOG(DEBUG) << "expiry ScheduleWrite for " << state.remote_address;
      flush_egress_for(state);
      return false;
    case QuicConnectionPImpl::ExpiryAction::IdleClose:
      LOG(DEBUG) << "expiry IdleClose for " << state.remote_address;
      return true;
    case QuicConnectionPImpl::ExpiryAction::Close:
      LOG(DEBUG) << "expiry Close for " << state.remote_address;
      flush_egress_for(state);
      return true;
  }
  return true;
}

void QuicServer::close(QuicConnectionId cid) {
  auto it = connections_.find(cid);
  if (it == connections_.end()) {
    LOG(WARNING) << "Can't find connection for closing " << cid;
    return;
  }
  auto state = it->second;
  LOG(INFO) << "Close connection: " << *state;
  if (state->temp_cid) {
    to_primary_cid_.erase(*state->temp_cid);
  }
  connections_.erase(it);
  callback_->on_closed(cid);
}

void QuicServer::alarm() {
  // FIXME: this could be problematic if we have thousands of connections
  // Maybe add heap like in IoWorker.cpp
  std::vector<QuicConnectionId> to_erase;
  for (auto &[cid, state] : connections_) {
    if (try_close(*state)) {
      to_erase.push_back(cid);
      if (state->temp_cid) {
        to_primary_cid_.erase(*state->temp_cid);
      }
      LOG(INFO) << "Close connection: " << *state;
    }
  }
  for (auto cid : to_erase) {
    connections_.erase(cid);
    callback_->on_closed(cid);
  }
  update_alarm();
}

void QuicServer::loop() {
  LOG(ERROR) << "unexpected loop signal";
}

void QuicServer::notify() {
  td::actor::send_signals(self_id_, td::actor::ActorSignals::wakeup());
}

void QuicServer::wake_up() {
  td::sync_with_poll(fd_);
  drain_ingress();
  flush_egress_all();
  update_alarm();
}

class QuicServer::PImplCallback final : public QuicConnectionPImpl::Callback {
 public:
  explicit PImplCallback(QuicServer::Callback &callback, bool is_outbound)
      : callback_(callback), is_outbound_(is_outbound) {
  }

  void set_connection_id(QuicConnectionId cid) override {
    cid_ = cid;
  }

  void on_handshake_completed(HandshakeCompletedEvent event) override {
    callback_.on_connected(cid_, std::move(event.peer_public_key), is_outbound_);
  }

  void on_stream_data(StreamDataEvent event) override {
    callback_.on_stream(cid_, event.sid, std::move(event.data), event.fin);
  }

 private:
  QuicServer::Callback &callback_;
  QuicConnectionId cid_;
  bool is_outbound_;
};

td::Result<std::shared_ptr<QuicServer::ConnectionState>> QuicServer::get_or_create_connection(
    const UdpMessageBuffer &msg_in) {
  TRY_RESULT(vc, VersionCid::from_datagram(td::Slice(msg_in.storage)));

  auto primary_cid = vc.dcid;
  if (auto it = to_primary_cid_.find(primary_cid); it != to_primary_cid_.end()) {
    primary_cid = it->second;
  }

  if (auto connection = find_connection(primary_cid)) {
    return connection;
  }

  // Create new connection to handle unknown inbound message
  TRY_RESULT(local_address, fd_.get_local_address());

  TRY_RESULT(p_impl, QuicConnectionPImpl::create_server(local_address, msg_in.address, server_key_, alpn_.as_slice(),
                                                        vc, std::make_unique<PImplCallback>(*this->callback_, false)));

  QuicConnectionId cid = p_impl->get_primary_scid();
  QuicConnectionId temp_cid = vc.dcid;

  auto state = std::make_shared<ConnectionState>(ConnectionState{.impl_ = std::move(p_impl),
                                                                 .remote_address = msg_in.address,
                                                                 .cid = cid,
                                                                 .temp_cid = temp_cid,
                                                                 .is_outbound = false});
  LOG(INFO) << "creating " << *state;

  // Store by BOTH current temporary dcid and cid we just generated for the server
  connections_[state->cid] = state;
  to_primary_cid_[*state->temp_cid] = state->cid;
  // TODO: remove by both cids

  return state;
}

td::Result<QuicConnectionId> QuicServer::connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key,
                                                 td::Slice alpn) {
  td::IPAddress remote_address;
  TRY_STATUS(remote_address.init_host_port(host.str(), port));
  TRY_RESULT(local_address, fd_.get_local_address());  // TODO: we may avoid system call here

  TRY_RESULT(p_impl, QuicConnectionPImpl::create_client(local_address, remote_address, std::move(client_key), alpn,
                                                        std::make_unique<PImplCallback>(*this->callback_, true)));
  QuicConnectionId cid = p_impl->get_primary_scid();

  auto state = std::make_shared<ConnectionState>(ConnectionState{
      .impl_ = std::move(p_impl), .remote_address = remote_address, .cid = cid, .temp_cid = {}, .is_outbound = true});
  LOG(INFO) << "creating " << *state;

  connections_[cid] = state;

  flush_egress_for(*state);
  update_alarm_for(*state);

  return QuicConnectionId(cid);
}

void QuicServer::drain_ingress() {
  td::PerfWarningTimer w("drain_ingress", 0.1);
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

    if (auto status = state->impl().handle_ingress(msg_in); status.is_error()) {
      LOG(WARNING) << "failed to handle ingress from " << *state << ":  " << status;
      close(state->cid);
      return td::Status::OK();
    }

    // TODO: do we need egress here, we will do flush_egress_all_soon anyway?
    flush_egress_for(*state);
    return td::Status::OK();
  };

  while (run) {
    if (auto status = cycle(); status.is_error()) {
      LOG(ERROR) << "failed to drain incoming traffic: " << status;
      break;
    }
  }
}

void QuicServer::flush_egress_for(ConnectionState &state, EgressData data) {
  td::PerfWarningTimer w("flush_egress_for", 0.1);
  if (data.stream_data.has_value()) {
    auto &stream_data = data.stream_data.value();
    auto cycle = [this, &state, &stream_data]() -> td::Status {
      char buf[DEFAULT_MTU];
      UdpMessageBuffer msg_out;
      msg_out.storage = td::MutableSlice(buf, DEFAULT_MTU);
      TRY_STATUS(state.impl().write_stream(msg_out, stream_data.sid, std::move(stream_data.data), stream_data.fin));

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
    TRY_STATUS(state.impl().produce_egress(msg_out));
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
  td::PerfWarningTimer w("flush_egress_all", 0.1);
  // TODO: could be optimized if we support list of connections with nonempty write
  for (auto &[cid, state] : connections_) {
    flush_egress_for(*state);
  }
}

void QuicServer::update_alarm() {
  td::PerfWarningTimer w("update_alarm", 0.1);
  td::Timestamp alarm_ts = td::Timestamp::never();
  for (auto &[cid, state] : connections_) {
    alarm_ts.relax(state->impl().get_expiry_timestamp());
  }
  alarm_timestamp() = alarm_ts;
}

void QuicServer::update_alarm_for(ConnectionState &state) {
  // Only decrease global timestamp for now.
  // May lead to waking up earlier than necessary, but usually correct
  alarm_timestamp().relax(state.impl().get_expiry_timestamp());
}

void QuicServer::send_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data) {
  send_stream(cid, sid, std::move(data), false);
}

void QuicServer::send_stream_end(QuicConnectionId cid, QuicStreamID sid) {
  send_stream(cid, sid, {}, true);
}

td::Result<QuicStreamID> QuicServer::open_stream(QuicConnectionId cid) {
  return send_stream(cid, {}, {}, false);
}

td::Result<QuicStreamID> QuicServer::send_stream(QuicConnectionId cid, std::optional<QuicStreamID> o_sid,
                                                 td::BufferSlice data, bool is_end) {
  auto state = find_connection(cid);
  if (!state) {
    return td::Status::Error("Connection not found");
  }

  QuicStreamID sid;
  if (o_sid) {
    sid = *o_sid;
  } else {
    TRY_RESULT_ASSIGN(sid, state->impl().open_stream());
  }

  if (!data.empty() || is_end) {
    flush_egress_for(*state,
                     {.stream_data = EgressData::StreamData{.sid = sid, .data = std::move(data), .fin = is_end}});
    update_alarm_for(*state);
  }

  return sid;
}

}  // namespace ton::quic
