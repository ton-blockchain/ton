#include <atomic>

#include "td/actor/actor.h"
#include "td/utils/Timer.h"

#include "quic-pimpl.h"
#include "quic-server.h"

namespace ton::quic {

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::create(int port, td::Ed25519::PrivateKey server_key,
                                                               std::unique_ptr<Callback> callback, td::Slice alpn,
                                                               td::Slice bind_host) {
  return create(port, std::move(server_key), std::move(callback), alpn, bind_host, Options{});
}

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::create(int port, td::Ed25519::PrivateKey server_key,
                                                               std::unique_ptr<Callback> callback, td::Slice alpn,
                                                               td::Slice bind_host, Options options) {
  CHECK(callback);
  td::IPAddress local_addr;
  TRY_STATUS(local_addr.init_host_port(bind_host.str(), port));

  TRY_RESULT(fd, td::UdpSocketFd::open(local_addr));

  auto name = PSTRING() << "QUIC:" << local_addr;
  return td::actor::create_actor<QuicServer>(td::actor::ActorOptions().with_name(name).with_poll(true), std::move(fd),
                                             std::move(server_key), td::BufferSlice(alpn), std::move(callback),
                                             options);
}

QuicServer::QuicServer(td::UdpSocketFd fd, td::Ed25519::PrivateKey server_key, td::BufferSlice alpn,
                       std::unique_ptr<Callback> callback, Options options)
    : fd_(std::move(fd))
    , alpn_(std::move(alpn))
    , server_key_(std::move(server_key))
    , gso_enabled_(options.enable_gso && td::UdpSocketFd::is_gso_supported())
    , cc_algo_(options.cc_algo)
    , flood_control_(options.flood_control)
    , callback_(std::move(callback)) {
  if (options.enable_gro) {
    auto gro_status = fd_.enable_gro();
    if (gro_status.is_ok()) {
      gro_enabled_ = true;
    } else {
      LOG(DEBUG) << "UDP_GRO not enabled: " << gro_status;
    }
  }
  if (!options.enable_mmsg) {
    fd_.disable_mmsg();
  } else {
    fd_.enable_mmsg();
  }
  LOG(INFO) << "UDP allowed: GRO=" << (gro_enabled_ ? "on" : "off") << " GSO=" << (gso_enabled_ ? "on" : "off")
            << " MMSG=" << (fd_.is_mmsg_enabled() ? "on" : "off") << " CC=" << cc_algo_;

  const size_t ingress_buf_size = gro_enabled_ ? kMaxDatagram : DEFAULT_MTU * kMaxBurst;
  ingress_buffers_.resize(kIngressBatch * ingress_buf_size);
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

void QuicServer::on_connection_updated(ConnectionState &state) {
  if (!state.in_active_queue) {
    state.in_active_queue = true;
    active_connections_.push_back(state.cid);
  }

  double key = state.impl().get_expiry_timestamp().at();
  if (state.in_heap()) {
    timeout_heap_.fix(key, &state);
  } else {
    timeout_heap_.insert(key, &state);
  }

  yield();
}

std::shared_ptr<QuicServer::ConnectionState> QuicServer::find_connection(const QuicConnectionId &cid) {
  if (auto it = connections_.find(cid); it != connections_.end()) {
    return it->second;
  }
  return nullptr;
}

bool QuicServer::handle_expiry(ConnectionState &state) {
  if (!state.impl().is_expired()) {
    on_connection_updated(state);
    return false;
  }
  auto R = state.impl().handle_expiry();
  if (R.is_error()) {
    LOG(INFO) << "expiry error: " << R.error();
    return true;
  }

  // TODO: close more explicitly?
  switch (R.ok()) {
    case QuicConnectionPImpl::ExpiryAction::None:
      LOG(DEBUG) << "expiry None for " << state.remote_address;
      return false;
    case QuicConnectionPImpl::ExpiryAction::ScheduleWrite:
      LOG(DEBUG) << "expiry ScheduleWrite for " << state.remote_address;
      on_connection_updated(state);
      return false;
    case QuicConnectionPImpl::ExpiryAction::IdleClose:
      LOG(INFO) << "expiry IdleClose for " << state.remote_address;
      return true;
    case QuicConnectionPImpl::ExpiryAction::Close:
      LOG(INFO) << "expiry Close for " << state.remote_address;
      on_connection_updated(state);  // should we?..
      return true;
  }
  return true;
}

void QuicServer::shutdown_stream(QuicConnectionId cid, QuicStreamID sid) {
  auto state = find_connection(cid);
  if (!state) {
    return;
  }
  state->impl().shutdown_stream(sid);
  on_connection_updated(*state);
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
  if (state->in_heap()) {
    timeout_heap_.erase(state.get());
  }
  if (flood_control_.has_value()) {
    auto flood_addr = state->remote_address.get_ip_host();
    if (flood_map_.contains(flood_addr) && --flood_map_.at(flood_addr) == 0) {
      flood_map_.erase(flood_addr);
    }
  }
  connections_.erase(it);
  callback_->on_closed(cid);
}

void QuicServer::alarm() {
  loop();
}
void QuicServer::handle_timeouts() {
  double now = td::Timestamp::now().at();
  while (!timeout_heap_.empty() && timeout_heap_.top_key() <= now) {
    auto *state = static_cast<ConnectionState *>(timeout_heap_.pop());
    if (handle_expiry(*state)) {
      to_erase_connections_.push_back(state->cid);
    }
  }

  StreamShutdownList shutdown;
  callback_->loop(td::Timestamp::now(), shutdown);
  for (auto &e : shutdown.entries) {
    shutdown_stream(e.cid, e.sid);
  }
}

void QuicServer::erase_pending_connections() {
  for (auto cid : to_erase_connections_) {
    close(cid);
  }
  to_erase_connections_.clear();
}

void QuicServer::log_stats(std::string reason) {
  LOG(INFO) << "quic stats (" << reason << "): udp ingress{syscalls=" << ingress_stats_.syscalls
            << " packets=" << ingress_stats_.packets << " bytes=" << ingress_stats_.bytes
            << "} egress{syscalls=" << egress_stats_.syscalls << " packets=" << egress_stats_.packets
            << " bytes=" << egress_stats_.bytes << "}";
  if (connections_.empty()) {
    return;
  }
  for (auto &[cid, state] : connections_) {
    log_conn_stats(*state, reason.c_str());
  }
}

void QuicServer::log_conn_stats(ConnectionState &state, const char *reason) {
  constexpr double kNsToMs = 1e-6;
  auto info = state.impl().get_conn_info();
  double loss_pct =
      info.pkt_sent ? (100.0 * static_cast<double>(info.pkt_lost) / static_cast<double>(info.pkt_sent)) : 0.0;
  LOG(INFO) << "quic stats (" << reason << ") for " << state.remote_address << " cid=" << state.cid
            << " rtt_ms{smoothed=" << static_cast<double>(info.smoothed_rtt) * kNsToMs
            << " min=" << static_cast<double>(info.min_rtt) * kNsToMs
            << " latest=" << static_cast<double>(info.latest_rtt) * kNsToMs
            << " var=" << static_cast<double>(info.rttvar) * kNsToMs << "}"
            << " cwnd=" << info.cwnd << " inflight=" << info.bytes_in_flight << " sent=" << info.pkt_sent << "/"
            << info.bytes_sent << " recv=" << info.pkt_recv << "/" << info.bytes_recv << " lost=" << info.pkt_lost
            << "/" << info.bytes_lost << " loss=" << loss_pct << "%";
}

void QuicServer::loop() {
  td::sync_with_poll(fd_);
  handle_timeouts();
  drain_ingress();
  flush_egress();
  erase_pending_connections();
  update_alarm();
}

void QuicServer::notify() {
  td::actor::send_signals(self_id_, td::actor::ActorSignals::wakeup());
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

  td::Status on_stream_data(StreamDataEvent event) override {
    return callback_.on_stream(cid_, event.sid, std::move(event.data), event.fin);
  }
  void on_stream_closed(QuicStreamID sid) override {
    return callback_.on_stream_closed(cid_, sid);
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

  auto flood_addr = msg_in.address.get_ip_host();
  if (flood_control_.has_value() && flood_map_.contains(flood_addr) && flood_map_.at(flood_addr) >= *flood_control_) {
    return td::Status::Error("flood control overflow");
  }

  // Create new connection to handle unknown inbound message
  TRY_RESULT(local_address, fd_.get_local_address());

  QuicConnectionOptions conn_options;
  conn_options.cc_algo = cc_algo_;
  TRY_RESULT(p_impl, QuicConnectionPImpl::create_server(local_address, msg_in.address, server_key_, alpn_.as_slice(),
                                                        vc, std::make_unique<PImplCallback>(*this->callback_, false),
                                                        conn_options));

  QuicConnectionId cid = p_impl->get_primary_scid();
  QuicConnectionId temp_cid = vc.dcid;

  auto state = std::make_shared<ConnectionState>(ConnectionState{
      .impl_ = std::move(p_impl),
      .remote_address = msg_in.address,
      .cid = cid,
      .temp_cid = temp_cid,
      .is_outbound = false,
  });
  LOG(INFO) << "creating " << *state;

  // Store by BOTH current temporary dcid and cid we just generated for the server
  connections_[state->cid] = state;
  to_primary_cid_[*state->temp_cid] = state->cid;
  // TODO: remove by both cids

  if (flood_control_.has_value()) {
    flood_map_[flood_addr]++;
  }

  return state;
}

td::Result<QuicConnectionId> QuicServer::connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key,
                                                 td::Slice alpn) {
  td::IPAddress remote_address;
  TRY_STATUS(remote_address.init_host_port(host.str(), port));
  TRY_RESULT(local_address, fd_.get_local_address());  // TODO: we may avoid system call here

  auto flood_addr = remote_address.get_ip_host();
  if (flood_control_.has_value() && flood_map_.contains(flood_addr) && flood_map_.at(flood_addr) >= *flood_control_) {
    return td::Status::Error("flood control overflow");
  }

  QuicConnectionOptions conn_options;
  conn_options.cc_algo = cc_algo_;
  TRY_RESULT(p_impl,
             QuicConnectionPImpl::create_client(local_address, remote_address, std::move(client_key), alpn,
                                                std::make_unique<PImplCallback>(*this->callback_, true), conn_options));
  QuicConnectionId cid = p_impl->get_primary_scid();

  auto state = std::make_shared<ConnectionState>(ConnectionState{
      .impl_ = std::move(p_impl),
      .remote_address = remote_address,
      .cid = cid,
      .temp_cid = {},
      .is_outbound = true,
  });
  LOG(INFO) << "creating " << *state;

  connections_[cid] = state;

  if (flood_control_.has_value()) {
    flood_map_[flood_addr]++;
  }

  on_connection_updated(*state);
  return QuicConnectionId(cid);
}

void QuicServer::drain_ingress() {
  td::PerfWarningTimer w("drain_ingress", 0.1);
  const size_t buf_size = gro_enabled_ ? kMaxDatagram : DEFAULT_MTU * kMaxBurst;

  std::vector<td::BufferSlice> ingress_data_buffers;  // for Windows receive_messages
  td::int64 bytes_budget = 10 << 20;                  // 10MB
  while (bytes_budget > 0) {
    for (size_t i = 0; i < kIngressBatch; i++) {
      ingress_errors_[i] = td::Status::OK();
      ingress_messages_[i].from = &ingress_msg_[i].address;
      ingress_messages_[i].data =
          td::MutableSlice(ingress_buffers_.data(), ingress_buffers_.size()).substr(i * buf_size, buf_size);
      ingress_messages_[i].error = &ingress_errors_[i];
    }

    size_t cnt = 0;
    auto status =
        fd_.receive_messages(td::MutableSpan<td::UdpSocketFd::InboundMessage>(ingress_messages_.data(), kIngressBatch),
                             cnt, ingress_data_buffers);
    if (cnt == 0) {
      if (status.is_error()) {
        LOG(ERROR) << "failed to drain incoming traffic: " << status;
      }
      break;
    }
    ingress_stats_.syscalls++;

    // Debug: log recvmmsg batch details periodically
    static std::atomic<size_t> ingress_log_counter = 0;
    if ((ingress_log_counter.fetch_add(1, std::memory_order_relaxed) & 0x3FFF) == 0) {
      std::map<QuicConnectionId, size_t> conn_to_idx;
      std::vector<size_t> packet_conn_idx(cnt);
      for (size_t i = 0; i < cnt; i++) {
        if (ingress_errors_[i].is_ok()) {
          auto vc = VersionCid::from_datagram(ingress_messages_[i].data);
          if (vc.is_ok()) {
            auto [it, inserted] = conn_to_idx.try_emplace(vc.ok().dcid, conn_to_idx.size());
            packet_conn_idx[i] = it->second;
          }
        }
      }
      td::StringBuilder sb;
      sb << "recvmmsg batch=" << cnt << " conns=" << conn_to_idx.size() << " [";
      for (size_t i = 0; i < cnt; i++) {
        if (i > 0) {
          sb << ", ";
        }
        sb << ingress_messages_[i].data.size();
        if (ingress_messages_[i].gso_size > 0) {
          sb << "(gro=" << ingress_messages_[i].gso_size << ")";
        }
        sb << "/c" << packet_conn_idx[i];
      }
      sb << "]";
      LOG(INFO) << sb.as_cslice();
    }

    for (size_t i = 0; i < cnt; i++) {
      if (ingress_errors_[i].is_error()) {
        LOG(DEBUG) << "dropping inbound packet from " << ingress_msg_[i].address << ": " << ingress_errors_[i];
        continue;
      }
      bytes_budget -= ingress_messages_[i].data.size();
      ingress_msg_[i].storage = ingress_messages_[i].data;
      ingress_stats_.bytes += ingress_msg_[i].storage.size();
      const size_t segment_size = ingress_messages_[i].gso_size;

      auto handle_packet = [&](UdpMessageBuffer &packet) {
        ingress_stats_.packets++;
        auto R = get_or_create_connection(packet);
        if (R.is_error()) {
          LOG(WARNING) << "dropping inbound packet from " << packet.address << ": " << R.error();
          return;
        }
        auto state = R.move_as_ok();
        if (auto handle_status = state->impl().handle_ingress(packet); handle_status.is_error()) {
          LOG(WARNING) << "failed to handle ingress from " << *state << ":  " << handle_status;
          close(state->cid);
          return;
        }
        on_connection_updated(*state);
      };

      if (segment_size > 0 && ingress_msg_[i].storage.size() > segment_size) {
        size_t offset = 0;
        while (offset < ingress_msg_[i].storage.size()) {
          const size_t len = td::min(segment_size, ingress_msg_[i].storage.size() - offset);
          UdpMessageBuffer segment = ingress_msg_[i];
          segment.storage = ingress_msg_[i].storage.substr(offset, len);
          handle_packet(segment);
          offset += len;
        }
      } else {
        handle_packet(ingress_msg_[i]);
      }
    }

    if (status.is_error()) {
      LOG(ERROR) << "failed to drain incoming traffic: " << status;
      break;
    }
  }
  if (bytes_budget <= 0) {
    yield();
  }
}

bool QuicServer::flush_pending() {
  if (pending_batch_count_ == 0) {
    return true;
  }

  size_t sent_count = 0;
  auto status =
      fd_.send_messages(td::Span<td::UdpSocketFd::OutboundMessage>(egress_messages_.data() + pending_batch_sent_,
                                                                   pending_batch_count_ - pending_batch_sent_),
                        sent_count);

  egress_stats_.syscalls++;
  for (size_t i = pending_batch_sent_; i < pending_batch_sent_ + sent_count; i++) {
    egress_stats_.bytes += egress_messages_[i].data.size();
    size_t gso_size = egress_messages_[i].gso_size;
    if (gso_size > 0 && egress_messages_[i].data.size() > gso_size) {
      egress_stats_.packets += (egress_messages_[i].data.size() + gso_size - 1) / gso_size;
    } else {
      egress_stats_.packets++;
    }
  }

  pending_batch_sent_ += sent_count;

  if (pending_batch_sent_ < pending_batch_count_) {
    if (status.is_error()) {
      LOG(WARNING) << "send_messages failed: " << status;
    }
    return false;  // blocked, will retry on wakeup
  }

  // All sent - re-queue connections for more data
  pending_batch_count_ = 0;
  pending_batch_sent_ = 0;
  return true;
}

bool QuicServer::produce_next_egress(size_t batch_index) {
  const size_t max_packets = gso_enabled_ ? kMaxBurst : 1;
  const size_t max_buf = DEFAULT_MTU * max_packets;

  while (!active_connections_.empty()) {
    auto cid = active_connections_.front();
    active_connections_.pop_front();

    auto conn = find_connection(cid);
    if (!conn) {
      continue;  // stale entry
    }

    conn->in_active_queue = false;

    auto &batch = egress_batches_[batch_index];
    batch.storage = td::MutableSlice(egress_buffers_[batch_index].data(), max_buf);

    auto status = conn->impl().produce_egress(batch, gso_enabled_, max_packets);
    if (status.is_error()) {
      LOG(WARNING) << "produce_egress failed for " << conn->remote_address << ": " << status;
      continue;
    }
    if (batch.storage.empty()) {
      continue;  // no data, connection stays out of queue
    }
    on_connection_updated(*conn);

    egress_batch_owners_[batch_index] = conn;
    return true;
  }
  return false;
}

void QuicServer::flush_egress() {
  td::PerfWarningTimer w("flush_egress_all", 0.1);

  // First flush any pending from previous call
  if (!flush_pending()) {
    return;  // still blocked
  }

  auto active_count = active_connections_.size();
  auto total_count = connections_.size();

  while (!active_connections_.empty()) {
    size_t batch_count = 0;
    while (batch_count < kEgressBatch && produce_next_egress(batch_count)) {
      batch_count++;
    }
    if (batch_count == 0) {
      break;
    }

    // Prepare messages
    for (size_t i = 0; i < batch_count; i++) {
      egress_messages_[i].to = &egress_batches_[i].address;
      egress_messages_[i].data = egress_batches_[i].storage;
      egress_messages_[i].gso_size = gso_enabled_ ? egress_batches_[i].gso_size : 0;
    }

    // Debug: log sendmmsg batch details (every 64 batches)
    static std::atomic<size_t> batch_log_counter = 0;
    if ((batch_log_counter.fetch_add(1, std::memory_order_relaxed) & 0x3FFF) == 0) {
      std::map<QuicConnectionId, size_t> conn_to_idx;
      std::vector<size_t> packet_conn_idx(batch_count);
      for (size_t i = 0; i < batch_count; i++) {
        auto [it, inserted] = conn_to_idx.try_emplace(egress_batch_owners_[i]->cid, conn_to_idx.size());
        packet_conn_idx[i] = it->second;
      }
      td::StringBuilder sb;
      sb << "sendmmsg batch=" << batch_count << " conns=" << conn_to_idx.size() << " [";
      for (size_t i = 0; i < batch_count; i++) {
        if (i > 0) {
          sb << ", ";
        }
        sb << egress_batches_[i].storage.size();
        if (egress_batches_[i].gso_size > 0) {
          sb << "(gso=" << egress_batches_[i].gso_size << ")";
        }
        sb << "/c" << packet_conn_idx[i] << "/s" << egress_batch_owners_[i]->impl().get_last_packet_streams();
      }
      sb << "] active/total=" << active_count << "/" << total_count;
      LOG(INFO) << sb.as_cslice();
    }

    // Set pending and try to flush
    pending_batch_count_ = batch_count;
    pending_batch_sent_ = 0;
    if (!flush_pending()) {
      return;  // blocked, will continue on next wakeup
    }
  }
}

void QuicServer::update_alarm() {
  td::Timestamp alarm_ts = td::Timestamp::never();
  if (!timeout_heap_.empty()) {
    alarm_ts = td::Timestamp::at(timeout_heap_.top_key());
  }
  alarm_ts.relax(callback_->next_alarm());
  alarm_timestamp() = alarm_ts;
}

void QuicServer::send_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data) {
  send_stream(cid, sid, std::move(data), false);
}

void QuicServer::send_stream_end(QuicConnectionId cid, QuicStreamID sid) {
  send_stream(cid, sid, {}, true);
}

td::Result<QuicStreamID> QuicServer::open_stream(QuicConnectionId cid, StreamOptions options) {
  return send_stream(cid, options, {}, false);
}

td::Result<QuicStreamID> QuicServer::send_stream(QuicConnectionId cid, std::variant<QuicStreamID, StreamOptions> stream,
                                                 td::BufferSlice data, bool is_end) {
  auto state = find_connection(cid);
  if (!state) {
    return td::Status::Error("Connection not found");
  }

  QuicStreamID sid;
  if (auto *existing = std::get_if<QuicStreamID>(&stream)) {
    sid = *existing;
  } else {
    TRY_RESULT_ASSIGN(sid, state->impl().open_stream());
    callback_->set_stream_options(cid, sid, std::get<StreamOptions>(stream));
  }

  TRY_STATUS(state->impl().buffer_stream(sid, std::move(data), is_end));
  on_connection_updated(*state);
  return sid;
}

void QuicServer::change_stream_options(QuicConnectionId cid, QuicStreamID sid, StreamOptions options) {
  callback_->set_stream_options(cid, sid, options);
}

}  // namespace ton::quic
