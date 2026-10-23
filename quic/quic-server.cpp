#include <atomic>
#include <chrono>
#include <utility>

#include "td/actor/actor.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/Timer.h"
#include "td/utils/algorithm.h"

#include "quic-pimpl.h"
#include "quic-server.h"

namespace ton::quic {

namespace {

constexpr ngtcp2_duration RETRY_TOKEN_TIMEOUT = 10 * NGTCP2_SECONDS;

ngtcp2_tstamp retry_token_now() {
  return static_cast<ngtcp2_tstamp>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::create(int port, std::unique_ptr<Callback> callback,
                                                               td::uint64 default_mtu, ServerIdentity identity,
                                                               td::Slice alpn, td::Slice bind_host) {
  return create(port, std::move(callback), default_mtu, std::move(identity), alpn, bind_host, Options{});
}

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::create(int port, std::unique_ptr<Callback> callback,
                                                               td::uint64 default_mtu, ServerIdentity identity,
                                                               td::Slice alpn, td::Slice bind_host, Options options,
                                                               FloodLimits flood_limits) {
  CHECK(callback);
  td::IPAddress local_addr;
  TRY_STATUS(local_addr.init_host_port(bind_host.str(), port));
  // Bind IPv4 only, so we never accept IPv6 peers: our per-IP flood tables (rate limiters,
  // half-open/connection caps, discourage) are keyed by address, and a peer with an IPv6 /64 could
  // rotate addresses to defeat every per-IP cap. Refusing the v6 bind is what keeps every peer
  // address IPv4 (an AF_INET socket can't receive v6) — until we aggregate per-IP caps by /64 prefix.
  if (!local_addr.is_ipv4()) {
    return td::Status::Error(PSLICE() << "QuicServer refuses non-IPv4 bind address " << bind_host);
  }

  TRY_RESULT(fd, td::UdpSocketFd::open(local_addr));

  auto name = PSTRING() << "QUIC:" << local_addr;
  return td::actor::create_actor<QuicServer>(td::actor::ActorOptions().with_name(name).with_poll(true), std::move(fd),
                                             default_mtu, std::move(identity), td::BufferSlice(alpn),
                                             std::move(callback), options, flood_limits);
}

QuicServer::QuicServer(td::UdpSocketFd fd, td::uint64 default_mtu, ServerIdentity identity, td::BufferSlice alpn,
                       std::unique_ptr<Callback> callback, Options options, FloodLimits flood_limits)
    : fd_(std::move(fd))
    , alpn_(std::move(alpn))
    , identities_(td::make_ref<ServerIdentities>())
    , options_(options)
    , guard_(flood_limits, *this)
    , gso_enabled_(options.enable_gso && td::UdpSocketFd::is_gso_supported())
    , callback_(std::move(callback)) {
  auto local_id = identity.local_id;
  CHECK(identities_.write().add_identity(std::move(identity)));
  set_default_mtu(local_id, default_mtu);
  td::Random::secure_bytes(td::MutableSlice(retry_secret_.data(), retry_secret_.size()));
  auto rcv_buffer = fd_.maximize_rcv_buffer();
  auto snd_buffer = fd_.maximize_snd_buffer();
  LOG(INFO) << "UDP buffers: rcv=" << (rcv_buffer.is_ok() ? rcv_buffer.move_as_ok() : 0)
            << " snd=" << (snd_buffer.is_ok() ? snd_buffer.move_as_ok() : 0);

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
            << " MMSG=" << (fd_.is_mmsg_enabled() ? "on" : "off") << " CC=" << options_.cc_algo
            << " Retry=" << (options_.stateless_retry ? "on" : "off");

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

std::deque<QuicConnectionId> &QuicServer::active_queue_for(const ConnectionState &state) {
  // Our own outbound dials are trusted-class from creation (is_outbound is stamped before the
  // handshake); inbound is untrusted until the handshake proves trust — the safe default.
  return (state.peer_info.trusted || state.is_outbound) ? active_trusted_ : active_untrusted_;
}

void QuicServer::on_connection_updated(ConnectionState &state) {
  if (!state.in_active_queue) {
    state.in_active_queue = true;
    active_queue_for(state).push_back(state.cid);
  }

  double key = state.impl().get_expiry_timestamp().at();
  if (state.in_heap()) {
    timeout_heap_.fix(key, &state);
  } else {
    timeout_heap_.insert(key, &state);
  }

  schedule_wakeup();
}

// yield() would end the turn here: the Yield flag counts as immediate, so ActorExecutor stops
// draining the mailbox after this one message and re-queues us. Every send_stream then got its own
// turn, its own loop(), and its own flush_egress -- so exactly one stream was ever ready when a
// packet was built, and ngtcp2 had nothing to coalesce despite being asked to. A Wakeup signal
// re-runs loop() just the same, but only once the mailbox is drained, which is what lets several
// messages share a datagram.
void QuicServer::schedule_wakeup() {
  request_loop();
}

void QuicServer::bind_cid(const QuicConnectionId &primary_cid, const QuicConnectionId &cid) {
  auto connection = find_connection(primary_cid);
  LOG_CHECK(connection) << "Can't bind CID for unknown primary cid " << primary_cid;
  LOG_CHECK(!cid_to_primary_cid_.contains(cid)) << "CID collision while binding " << cid << " to " << primary_cid;

  auto [_, routed_inserted] = connection->routed_cids.insert(cid);
  LOG_CHECK(routed_inserted) << "Duplicate routed CID " << cid << " for primary " << primary_cid;

  auto [__, mapping_inserted] = cid_to_primary_cid_.emplace(cid, primary_cid);
  LOG_CHECK(mapping_inserted) << "Failed to insert CID mapping " << cid << " -> " << primary_cid;
}

void QuicServer::unbind_cid(const QuicConnectionId &primary_cid, const QuicConnectionId &cid) {
  auto connection = find_connection(primary_cid);
  LOG_CHECK(connection) << "Can't unbind CID for unknown primary cid " << primary_cid;

  auto it = cid_to_primary_cid_.find(cid);
  LOG_CHECK(it != cid_to_primary_cid_.end()) << "Missing CID mapping for " << cid;
  LOG_CHECK(it->second == primary_cid) << "CID " << cid << " is mapped to " << it->second << ", not " << primary_cid;

  auto erased = connection->routed_cids.erase(cid);
  LOG_CHECK(erased == 1) << "Missing routed CID " << cid << " for primary " << primary_cid;
  cid_to_primary_cid_.erase(it);
}

void QuicServer::unbind_all_cids(ConnectionState &state) {
  if (state.bootstrap_routed_cid.has_value()) {
    auto bootstrap_it = bootstrap_routes_.find(
        BootstrapRouteKey{.remote_address = state.remote_address, .routed_cid = *state.bootstrap_routed_cid});
    LOG_CHECK(bootstrap_it != bootstrap_routes_.end())
        << "Missing bootstrap route " << *state.bootstrap_routed_cid << " from " << state.remote_address;
    LOG_CHECK(bootstrap_it->second == state.cid)
        << "Bootstrap route " << *state.bootstrap_routed_cid << " from " << state.remote_address << " is mapped to "
        << bootstrap_it->second << ", not " << state.cid;
    bootstrap_routes_.erase(bootstrap_it);
  }
  for (const auto &cid : state.routed_cids) {
    auto it = cid_to_primary_cid_.find(cid);
    LOG_CHECK(it != cid_to_primary_cid_.end()) << "Missing CID mapping for " << cid;
    LOG_CHECK(it->second == state.cid) << "CID " << cid << " is mapped to " << it->second << ", not " << state.cid;
    cid_to_primary_cid_.erase(it);
  }
  state.routed_cids.clear();
}

td::Result<std::shared_ptr<QuicServer::ConnectionState>> QuicServer::install_connection(
    std::unique_ptr<QuicConnectionPImpl> p_impl, const td::IPAddress &remote_address, bool is_outbound,
    std::optional<QuicConnectionId> bootstrap_routed_cid) {
  TRY_RESULT(initial_cid_state, p_impl->take_initial_cid_state());

  auto state = std::make_shared<ConnectionState>(ConnectionState{
      .impl_ = std::move(p_impl),
      .remote_address = remote_address,
      .cid = initial_cid_state.primary_scid,
      .local_id = {},
      .peer_id = {},
      .peer_info = {},
      .bootstrap_routed_cid = {},
      .routed_cids = {},
      .is_outbound = is_outbound,
  });
  LOG(INFO) << "creating " << *state;

  auto [_, inserted] = connections_.emplace(state->cid, state);
  LOG_CHECK(inserted) << "Duplicate primary CID " << state->cid;

  if (bootstrap_routed_cid.has_value()) {
    auto [__, bootstrap_inserted] = bootstrap_routes_.emplace(
        BootstrapRouteKey{.remote_address = remote_address, .routed_cid = *bootstrap_routed_cid}, state->cid);
    LOG_CHECK(bootstrap_inserted) << "Duplicate bootstrap route " << *bootstrap_routed_cid << " from "
                                  << remote_address;
    state->bootstrap_routed_cid = bootstrap_routed_cid;
  }

  std::set<QuicConnectionId> startup_cids;
  startup_cids.insert(state->cid);
  for (const auto &local_cid : initial_cid_state.scids) {
    startup_cids.insert(local_cid);
  }

  for (const auto &cid : startup_cids) {
    bind_cid(state->cid, cid);
  }

  return state;
}

QuicConnectionOptions QuicServer::build_connection_options() const {
  const auto &limits = guard_.limits();
  QuicConnectionOptions conn_options;
  conn_options.cc_algo = options_.cc_algo;
  // One limit bounds buffered bytes in both directions: our unacked egress is capped at the same
  // per-connection budget as inbound reassembly. Enforced on untrusted, log-only otherwise.
  conn_options.max_outbound_buffered_bytes = limits.max_connection_buffered;
  conn_options.handshake_timeout = limits.handshake_deadline > 0
                                       ? static_cast<ngtcp2_duration>(limits.handshake_deadline * NGTCP2_SECONDS)
                                       : std::numeric_limits<ngtcp2_duration>::max();
  if (options_.max_streams_bidi.has_value()) {
    conn_options.max_streams_bidi = *options_.max_streams_bidi;
  }
  if (options_.max_streams_uni.has_value()) {
    conn_options.max_streams_uni = *options_.max_streams_uni;
  }
  if (options_.ack_thresh.has_value()) {
    conn_options.ack_thresh = *options_.ack_thresh;
  }
  if (options_.max_ack_delay_seconds.has_value()) {
    conn_options.max_ack_delay = static_cast<ngtcp2_duration>(*options_.max_ack_delay_seconds * NGTCP2_SECONDS);
  }
  conn_options.max_datagram_frame_size = options_.max_datagram_frame_size;
  return conn_options;
}

void QuicServer::on_local_cid_issued(const QuicConnectionId &primary_cid, const QuicConnectionId &cid) {
  bind_cid(primary_cid, cid);
}

void QuicServer::on_local_cid_retired(const QuicConnectionId &primary_cid, const QuicConnectionId &cid) {
  unbind_cid(primary_cid, cid);
}

std::optional<ServerInitialInfo> QuicServer::prepare_server_initial_info(const VersionCid &initial_packet,
                                                                         const td::IPAddress &remote_address) {
  ServerInitialInfo initial_info{
      .packet = initial_packet,
      .original_dcid = initial_packet.dcid,
      .retry_scid = std::nullopt,
  };

  if (!options_.stateless_retry) {
    return initial_info;
  }

  if (initial_packet.token.empty()) {
    // Stage-A overload valve: under global saturation, drop instead of inviting a handshake we can't
    // admit. This only peeks the global limiter (the src IP is unproven here, so nothing is spent);
    // a Retry is amplification-safe, so absent saturation we always answer with one.
    if (!guard_.accepting_handshakes()) {
      record_transport_dropped(metrics::Direction::in, metrics::Reason::limited);
      return {};
    }
    send_retry(initial_packet, remote_address);
    return {};
  }

  auto original_dcid = verify_retry_token(initial_packet, remote_address);
  if (original_dcid.is_error()) {
    // Rejected here rather than by returning an error, so it is the only place that can count it.
    record_transport_dropped(metrics::Direction::in, metrics::Reason::invalid);
    LOG(DEBUG) << "invalid Retry token from " << remote_address << ": " << original_dcid.error();
    send_invalid_token_connection_close(initial_packet, remote_address);
    return {};
  }

  initial_info.original_dcid = original_dcid.move_as_ok();
  initial_info.retry_scid = initial_packet.dcid;
  return initial_info;
}

td::Result<QuicConnectionId> QuicServer::verify_retry_token(const VersionCid &packet,
                                                            const td::IPAddress &remote_address) const {
  CHECK(!packet.token.empty());

  auto packet_dcid = QuicConnectionIdAccess::to_ngtcp2(packet.dcid);
  ngtcp2_cid original_dcid{};
  int rv = ngtcp2_crypto_verify_retry_token2(
      &original_dcid, reinterpret_cast<const uint8_t *>(packet.token.data()), packet.token.size(), retry_secret_.data(),
      retry_secret_.size(), packet.version, reinterpret_cast<const ngtcp2_sockaddr *>(remote_address.get_sockaddr()),
      static_cast<ngtcp2_socklen>(remote_address.get_sockaddr_len()), &packet_dcid, RETRY_TOKEN_TIMEOUT,
      retry_token_now());
  switch (rv) {
    case 0:
      return QuicConnectionIdAccess::from_ngtcp2(original_dcid);
    case NGTCP2_CRYPTO_ERR_VERIFY_TOKEN:
      return td::Status::Error("retry token verification failed");
    case NGTCP2_CRYPTO_ERR_UNREADABLE_TOKEN:
      return td::Status::Error("retry token is unreadable");
    default:
      return td::Status::Error(PSTRING() << "retry token validation failed: " << rv);
  }
}

void QuicServer::send_stateless_datagram(td::Slice packet_kind, const td::IPAddress &remote_address, td::Slice data) {
  td::UdpSocketFd::OutboundMessage message{.to = &remote_address, .data = data, .gso_size = 0};
  bool is_sent = false;
  auto status = fd_.send_message(message, is_sent);
  reflect_socket_syscalls();
  auto &egress = udp_wire_.dir.at(metrics::Direction::out);
  if (is_sent) {
    egress.data.record(data.size());
  } else {
    // Stateless packets aren't retried, so a full send queue loses them just like a kernel refusal.
    egress.dropped.at(status.is_error() ? metrics::Reason::internal : metrics::Reason::limited).inc();
    LOG(DEBUG) << "dropping stateless " << packet_kind << " to " << remote_address << ": "
               << (status.is_error() ? td::Slice(status.message()) : td::Slice("send queue blocked"));
  }
}

void QuicServer::send_retry(const VersionCid &packet, const td::IPAddress &remote_address) {
  // The datagram itself was fine, so failing to build its Retry drops it on our own account.
  auto unbuilt = [&](td::Slice reason) {
    record_transport_dropped(metrics::Direction::in, metrics::Reason::internal);
    LOG(WARNING) << "failed to build Retry for " << remote_address << ": " << reason;
  };

  auto client_scid = QuicConnectionIdAccess::to_ngtcp2(packet.scid);
  auto original_dcid = QuicConnectionIdAccess::to_ngtcp2(packet.dcid);
  auto retry_scid = QuicConnectionIdAccess::to_ngtcp2(QuicConnectionId::random());

  std::array<uint8_t, NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2> token;
  auto tokenlen = ngtcp2_crypto_generate_retry_token2(
      token.data(), retry_secret_.data(), retry_secret_.size(), packet.version,
      reinterpret_cast<const ngtcp2_sockaddr *>(remote_address.get_sockaddr()),
      static_cast<ngtcp2_socklen>(remote_address.get_sockaddr_len()), &retry_scid, &original_dcid, retry_token_now());
  if (tokenlen < 0) {
    return unbuilt("failed to generate retry token");
  }

  std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> datagram;
  auto datagram_size = ngtcp2_crypto_write_retry(datagram.data(), datagram.size(), packet.version, &client_scid,
                                                 &retry_scid, &original_dcid, token.data(), tokenlen);
  if (datagram_size < 0) {
    return unbuilt("failed to write retry packet");
  }

  LOG(DEBUG) << "sending Retry to " << remote_address << " for original dcid=" << packet.dcid;
  send_stateless_datagram(
      "Retry", remote_address,
      td::Slice(reinterpret_cast<const char *>(datagram.data()), static_cast<size_t>(datagram_size)));
}

void QuicServer::send_invalid_token_connection_close(const VersionCid &packet, const td::IPAddress &remote_address) {
  auto client_scid = QuicConnectionIdAccess::to_ngtcp2(packet.scid);
  auto original_dcid = QuicConnectionIdAccess::to_ngtcp2(packet.dcid);

  std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> datagram;
  auto datagram_size = ngtcp2_crypto_write_connection_close(
      datagram.data(), datagram.size(), packet.version, &client_scid, &original_dcid, NGTCP2_INVALID_TOKEN, nullptr, 0);
  if (datagram_size < 0) {
    // The reject that got us here already accounted for the datagram, so this only needs a log.
    LOG(WARNING) << "failed to build invalid-token close for " << remote_address;
    return;
  }

  LOG(DEBUG) << "sending invalid-token connection close to " << remote_address;
  send_stateless_datagram(
      "invalid-token connection close", remote_address,
      td::Slice(reinterpret_cast<const char *>(datagram.data()), static_cast<size_t>(datagram_size)));
}

// Fail the peer fast on a close it did not initiate: without a CONNECTION_CLOSE it would keep
// using the connection until its own timeouts fire.
void QuicServer::send_close_notice(ConnectionState &state, CloseCode code) {
  std::array<char, NGTCP2_MAX_UDP_PAYLOAD_SIZE> close_buffer;
  UdpMessageBuffer close_msg;
  close_msg.storage = td::MutableSlice(close_buffer.data(), close_buffer.size());
  state.impl().write_application_close(close_msg, static_cast<td::uint64>(code), close_code_name(code));
  send_connection_close(state, close_msg);  // no-op when empty
}

// ---- FloodGuard::Host: the transport seam the guard drives, mapping verdicts onto existing code.
void QuicServer::drop_connection(QuicConnectionId cid, CloseCode code) {
  auto state = find_connection(cid);
  if (!state) {
    return;
  }
  send_close_notice(*state, code);
  on_connection_closed(cid);
}

void QuicServer::reset_stream(QuicConnectionId cid, QuicStreamID sid) {
  shutdown_stream(cid, sid);
}

void QuicServer::deliver(QuicConnectionId cid, QuicStreamID sid, td::Result<td::BufferSlice> message) {
  callback_->on_message(cid, sid, std::move(message));
}

void QuicServer::configure(QuicConnectionId cid, FloodGuard::TransportCaps caps) {
  auto state = find_connection(cid);
  if (!state) {
    return;
  }
  state->impl().set_outbound_buffered_cap(caps.egress_cap);
  state->impl().set_outbound_cap_enforced(caps.egress_enforced);
  if (caps.shape_ingress) {
    state->impl().start_ingress_shaping(&guard_.ingress_aggregate());
  } else {
    state->impl().stop_ingress_shaping();  // dumps accrued debt as an immediate offset extension
  }
  on_connection_updated(*state);  // flush any resulting MAX_DATA change
}

FloodGuard::IngressGrant QuicServer::grant_ingress(QuicConnectionId cid, td::Timestamp now, td::uint64 cap) {
  auto state = find_connection(cid);
  if (!state) {
    return {0, 0};  // gone == fully served
  }
  auto granted = state->impl().grant_ingress(now, cap);
  if (granted) {
    on_connection_updated(*state);
  }
  return {granted, state->impl().ingress_debt()};
}

void QuicServer::send_connection_close(ConnectionState &state, const UdpMessageBuffer &msg) {
  if (msg.storage.empty()) {
    return;
  }

  LOG(DEBUG) << "sending connection close to " << state.remote_address;
  send_stateless_datagram("connection close", msg.address, msg.storage);
}

std::shared_ptr<QuicServer::ConnectionState> QuicServer::find_connection(const QuicConnectionId &cid) {
  if (auto it = connections_.find(cid); it != connections_.end()) {
    return it->second;
  }
  return nullptr;
}

void QuicServer::handle_expiry(ConnectionState &state) {
  if (!state.impl().is_expired()) {
    on_connection_updated(state);
    return;
  }
  auto R = state.impl().handle_expiry();
  if (R.is_error()) {
    LOG(INFO) << "expiry error: " << R.error();
    to_erase_connections_.push_back({state.cid, true});
    return;
  }

  // TODO: close more explicitly?
  switch (R.ok()) {
    case QuicConnectionPImpl::ExpiryAction::None:
      LOG(DEBUG) << "expiry None for " << state.remote_address;
      return;
    case QuicConnectionPImpl::ExpiryAction::ScheduleWrite:
      LOG(DEBUG) << "expiry ScheduleWrite for " << state.remote_address;
      on_connection_updated(state);
      return;
    case QuicConnectionPImpl::ExpiryAction::IdleClose:
      LOG(INFO) << "expiry IdleClose for " << state.remote_address;
      to_erase_connections_.push_back({state.cid, false});  // RFC 9000 10.1: idle timeout closes silently
      return;
    case QuicConnectionPImpl::ExpiryAction::HandshakeTimeout:
      LOG(INFO) << "expiry HandshakeTimeout for " << state.remote_address;
      transport_stats_.handshakes.at(direction_of(state.is_outbound), HandshakeResult::timed_out).inc();
      break;
    case QuicConnectionPImpl::ExpiryAction::Close:
      LOG(INFO) << "expiry Close for " << state.remote_address;
      break;
  }
  to_erase_connections_.push_back({state.cid, true});
}

void QuicServer::shutdown_stream(QuicConnectionId cid, QuicStreamID sid) {
  guard_.on_stream_finished(cid, sid);  // the app's last word on it (release the in-flight charge)
  auto state = find_connection(cid);
  if (!state) {
    return;
  }
  state->impl().shutdown_stream(sid);
  on_connection_updated(*state);
  guard_.on_ingress_debt(cid, state->impl().ingress_debt());
}

void QuicServer::release_peer_uni_stream_credit(QuicConnectionId cid) {
  auto state = find_connection(cid);
  if (!state) {
    return;  // the connection is gone and its credit with it
  }
  // Only the release that actually announces credit needs the connection back in the egress
  // rotation; the batched ones in between have nothing to put on the wire.
  if (!state->impl().release_peer_uni_stream_credit()) {
    return;
  }
  on_connection_updated(*state);
}

td::actor::Task<ServerStats> QuicServer::collect() {
  reflect_socket_syscalls();
  // Only the socket knows how many datagrams the receive queue lost; fold in what accrued since the
  // last scrape.
  auto rx_drops = fd_.get_rx_queue_drops();
  if (rx_drops > rx_queue_drops_reflected_) {
    auto &ingress = udp_wire_.dir.at(metrics::Direction::in);
    ingress.dropped.at(metrics::Reason::limited).inc(rx_drops - rx_queue_drops_reflected_);
    rx_queue_drops_reflected_ = rx_drops;
  }

  auto summary = closed_conn_stats_;
  for (auto &[id, conn] : connections_) {
    summary += QuicConnectionMetricsAggregate::from_one(conn->impl_->get_stats(transport_stats_), conn->is_outbound);
  }

  // get_stats updates transport_stats_, so we snapshot after the loop above.
  co_return {
      .wire = udp_wire_,
      .transport =
          {
              .summary = summary,
              .stats = transport_stats_,
          },
      .batching = batching_,
      .flood = guard_.stats(),
  };
}

void QuicServer::on_connection_closed(QuicConnectionId cid) {
  auto it = connections_.find(cid);
  if (it == connections_.end()) {
    LOG(WARNING) << "Can't find connection for closing " << cid;
    return;
  }
  auto state = it->second;
  LOG(INFO) << "Close connection: " << *state;
  closed_conn_stats_ +=
      QuicConnectionMetricsAggregate::from_one(state->impl_->get_stats(transport_stats_), state->is_outbound).retire();
  unbind_all_cids(*state);
  guard_.on_connection_closed(cid);  // releases the guard's flood bookkeeping; idempotent
  if (state->in_heap()) {
    timeout_heap_.erase(state.get());
  }
  if (auto local_it = conns_by_peer_.find(state->local_id); local_it != conns_by_peer_.end()) {
    if (auto peer_it = local_it->second.find(state->peer_id); peer_it != local_it->second.end()) {
      peer_it->second.erase(state->cid);
      if (peer_it->second.empty()) {
        local_it->second.erase(peer_it);
      }
      if (local_it->second.empty()) {
        conns_by_peer_.erase(local_it);
      }
    }
  }
  connections_.erase(it);
  callback_->on_closed(cid);
}

void QuicServer::close_connection(QuicConnectionId cid, CloseCode code) {
  guard_.close_connection(cid, code);  // records the discourage verdict, then drops via Host::drop_connection
}

void QuicServer::set_reserved_only(bool reserved_only) {
  guard_.set_reserved_only(reserved_only);
  update_alarm();
}

void QuicServer::alarm() {
  loop();
}
void QuicServer::handle_timeouts() {
  double now = td::Timestamp::now().at();
  while (!timeout_heap_.empty() && timeout_heap_.top_key() <= now) {
    handle_expiry(*static_cast<ConnectionState *>(timeout_heap_.pop()));
  }
  // A queued datagram must not revive a connection that just timed out.
  erase_pending_connections();

  {
    td::PerfWarningTimer w("flood_guard_tick", 0.1);
    guard_.tick(td::Timestamp::now());  // stream deadlines, queued closes, evictions, limiter cleanup
  }
}

void QuicServer::erase_pending_connections() {
  // Take the list first: closing runs callbacks that may queue further erasures.
  for (auto [cid, notify_peer] : std::exchange(to_erase_connections_, {})) {
    if (auto state = find_connection(cid)) {
      if (notify_peer) {
        send_close_notice(*state, CloseCode::ok);
      }
      on_connection_closed(cid);
    }
  }
}

void QuicServer::log_stats(std::string reason) {
  reflect_socket_syscalls();
  const auto &ingress = udp_wire_.dir.at(metrics::Direction::in);
  const auto &egress = udp_wire_.dir.at(metrics::Direction::out);
  LOG(INFO) << "quic stats (" << reason << "): udp ingress{syscalls=" << ingress.syscalls.value()
            << " packets=" << ingress.data.packets.value() << " bytes=" << ingress.data.bytes.value()
            << "} egress{syscalls=" << egress.syscalls.value() << " packets=" << egress.data.packets.value()
            << " bytes=" << egress.data.bytes.value() << "}";
  for (auto &[cid, state] : connections_) {
    log_conn_stats(*state, reason.c_str());
  }
}

void QuicServer::set_default_mtu(adnl::AdnlNodeIdShort local_id, td::uint64 mtu) {
  if (mtu == 0) {
    default_mtu_by_local_id_.erase(local_id);
  } else {
    default_mtu_by_local_id_[local_id] = mtu;
  }
  refresh_peer_infos(local_id, {});
}

void QuicServer::set_peer_mtu(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id, td::uint64 mtu,
                              bool trusted) {
  if (mtu == 0) {
    peers_mtu_.erase({local_id, peer_id});
  } else {
    peers_mtu_[{local_id, peer_id}] = {mtu, trusted};
  }
  refresh_peer_infos(local_id, peer_id);
}

static td::Result<adnl::AdnlNodeIdShort> adnl_short_id_from_public_key(td::Slice public_key) {
  if (public_key.size() != 32) {
    return td::Status::Error("public key must be 32 bytes");
  }
  td::Bits256 key_bits;
  key_bits.as_slice().copy_from(public_key);
  return adnl::AdnlNodeIdFull(PublicKey(pubkeys::Ed25519(key_bits))).compute_short_id();
}

// The handshake proved the keys: stamp the connection with its identity and the registry's
// {mtu, trusted} for that path, then hand the connection to the application callback.
td::Status QuicServer::on_handshake_completed(QuicConnectionId cid, td::Slice local_public_key,
                                              td::Slice peer_public_key, bool is_outbound,
                                              metrics::Reason &reject_reason) {
  TRY_RESULT(local_id, adnl_short_id_from_public_key(local_public_key));
  TRY_RESULT(peer_id, adnl_short_id_from_public_key(peer_public_key));
  auto state = find_connection(cid);
  if (!state) {
    return td::Status::Error("connection is gone");
  }
  state->local_id = local_id;
  state->peer_id = peer_id;
  state->peer_info = resolve_peer_info(local_id, peer_id);
  // Classify into the trust class (egress cap, ingress shaping) and leave the half-open tables. A
  // policy refusal returns an error the caller routes through the failed-handshake teardown (a
  // non-discouraging ok-close), without announcing the connection to the application.
  auto admitted = !is_outbound && state->peer_info.mtu == 0
                      ? td::Status::Error("incoming connections are not accepted on this path")
                      : guard_.on_handshake_done(cid, state->peer_info.trusted, state->peer_info.mtu);
  if (admitted.is_error()) {
    reject_reason = metrics::Reason::limited;
    return admitted;
  }
  conns_by_peer_[local_id][peer_id].insert(state->cid);
  return callback_->on_connected(cid, local_id, peer_id, is_outbound, state->peer_info);
}

QuicServer::PeerMtuInfo QuicServer::resolve_peer_info(adnl::AdnlNodeIdShort local_id,
                                                      adnl::AdnlNodeIdShort peer_id) const {
  if (auto it = peers_mtu_.find({local_id, peer_id}); it != peers_mtu_.end()) {
    return it->second;
  }
  if (auto it = default_mtu_by_local_id_.find(local_id); it != default_mtu_by_local_id_.end()) {
    return {it->second, false};
  }
  return {};
}

void QuicServer::refresh_peer_info(ConnectionState &state) {
  auto info = resolve_peer_info(state.local_id, state.peer_id);
  if (info == state.peer_info) {
    return;
  }
  state.peer_info = info;
  guard_.on_peer_info_updated(state.cid, info.trusted, info.mtu);  // re-cap, re/un-shape on a trust flip
  callback_->on_peer_info_updated(state.cid, info);
}

void QuicServer::refresh_peer_infos(adnl::AdnlNodeIdShort local_id, std::optional<adnl::AdnlNodeIdShort> peer_id) {
  auto local_it = conns_by_peer_.find(local_id);
  if (local_it == conns_by_peer_.end()) {
    return;
  }
  auto refresh_cids = [&](const std::set<QuicConnectionId> &cids) {
    for (auto cid : cids) {
      if (auto state = find_connection(cid)) {
        refresh_peer_info(*state);
      }
    }
  };
  if (peer_id) {
    if (auto peer_it = local_it->second.find(*peer_id); peer_it != local_it->second.end()) {
      refresh_cids(peer_it->second);
    }
  } else {
    for (auto &[pid, cids] : local_it->second) {
      refresh_cids(cids);
    }
  }
  update_alarm();  // a live downgrade may have queued a reserved-only close
}

void QuicServer::add_identity(adnl::AdnlNodeIdShort local_id, td::Ed25519::PrivateKey key) {
  CHECK(identities_.not_null());
  if (!identities_.write().add_identity(ServerIdentity{.local_id = local_id, .key = std::move(key)})) {
    return;
  }
  LOG(INFO) << "QuicServer: registered identity " << local_id << " (now hosting " << identities_->by_sni.size()
            << " identities)";
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
  guard_.drain_shaper(td::Timestamp::now());  // internally paced: a no-op before the drain tick
  flush_egress();
  erase_pending_connections();
  update_alarm();
}

void QuicServer::notify() {
  td::actor::send_signals(self_id_, td::actor::ActorSignals::wakeup());
}

class QuicServer::PImplCallback final : public QuicConnectionPImpl::Callback {
 public:
  explicit PImplCallback(QuicServer &server, bool is_outbound)
      : server_(server), callback_(*server.callback_), is_outbound_(is_outbound) {
  }

  void set_connection_id(QuicConnectionId cid) override {
    cid_ = cid;
  }
  void on_local_cid_issued(QuicConnectionId cid) override {
    server_.on_local_cid_issued(cid_, cid);
  }
  void on_local_cid_retired(QuicConnectionId cid) override {
    server_.on_local_cid_retired(cid_, cid);
  }

  void on_handshake_completed(HandshakeCompletedEvent event) override {
    auto reject_reason = metrics::Reason::invalid;
    auto status = server_.on_handshake_completed(cid_, event.local_public_key.as_slice(),
                                                 event.peer_public_key.as_slice(), is_outbound_, reject_reason);
    if (status.is_error()) {
      // ngtcp2 never sees this failure (the callback returns 0 either way), so this is the only place
      // a synchronously rejected handshake is counted. A callback that defers its verdict to another
      // actor returns OK here and reports its own outcome via record_handshake_{reject,completed}().
      server_.record_handshake_reject(reject_reason, is_outbound_);
      LOG(WARNING) << "on_connected failed for " << cid_ << ": " << status;
      server_.to_erase_connections_.push_back({cid_, true});
    }
  }

  td::Status on_stream_open(QuicStreamID sid) override {
    return server_.guard_.on_peer_stream_open(cid_, sid);
  }

  td::Status on_stream_data(StreamDataEvent event) override {
    // An RFC 9221 datagram (pseudo-sid -1) is not a stream: it arrives whole, ngtcp2 already
    // bounded its size, and it holds no reassembly state — so stream budgets and churn do not
    // apply, and running it through the guard would count each datagram as stream abuse.
    if (event.sid < 0) {
      server_.deliver(cid_, event.sid, std::move(event.data));
      return td::Status::OK();
    }
    // The guard reassembles and flood-accounts; an error resets only this stream (never the
    // connection), and the failure has already been delivered to the app via Host::deliver.
    return server_.guard_.on_stream_data(cid_, event.sid, std::move(event.data), event.fin);
  }
  void on_stream_closed(StreamCloseEvent event) override {
    server_.guard_.on_stream_closed(cid_, event);  // churn / orphan re-homing
    callback_.on_stream_closed(cid_, event);
  }

 private:
  QuicServer &server_;
  QuicServer::Callback &callback_;
  QuicConnectionId cid_;
  bool is_outbound_;
};

// Nothing here can fail on the peer's account: a failure means we dropped a valid datagram ourselves,
// which is why the caller can classify the whole phase as `internal`.
td::Result<std::shared_ptr<QuicServer::ConnectionState>> QuicServer::create_inbound_connection(
    const UdpMessageBuffer &msg_in, const VersionCid &initial_packet, const ServerInitialInfo &initial_info) {
  TRY_RESULT(local_address, fd_.get_local_address());
  // The window is left auto-tuning (not pinned): the shaper bounds the *sustained* ingress rate by
  // pacing connection-level MAX_DATA offset extension, independent of window size. Auto-tuning only
  // fires in reaction to the credit we already granted, and the extra headroom it can inject over a
  // connection's whole life is bounded by max_window - initial_max_data — a one-time burst, not a
  // rate leak. Pinning the window to initial_max_data would have capped every inbound connection
  // (trusted included, since trust is unknown here) at ~initial_max_data per RTT.
  auto pimpl_callback = std::make_unique<PImplCallback>(*this, false);
  TRY_RESULT(p_impl,
             QuicConnectionPImpl::create_server(local_address, msg_in.address, identities_, alpn_.as_slice(),
                                                initial_info, std::move(pimpl_callback), build_connection_options()));
  return install_connection(std::move(p_impl), msg_in.address, false, initial_packet.dcid);
}

// A dropped inbound datagram, classified for quic_transport_dropped_total: invalid = unroutable or
// unparseable (peer fault); limited = a local flood/rate cap; internal = our own machinery failed.
td::Status QuicServer::reject(metrics::Reason reason, td::Status status) {
  record_transport_dropped(metrics::Direction::in, reason);
  return status;
}

td::Result<std::shared_ptr<QuicServer::ConnectionState>> QuicServer::get_or_create_connection(
    const UdpMessageBuffer &msg_in) {
  auto r_vc = VersionCid::from_datagram(td::Slice(msg_in.storage));
  if (r_vc.is_error()) {
    return reject(metrics::Reason::invalid, r_vc.move_as_error());
  }
  auto vc = r_vc.move_as_ok();

  if (auto it = cid_to_primary_cid_.find(vc.dcid); it != cid_to_primary_cid_.end()) {
    auto connection = find_connection(it->second);
    LOG_CHECK(connection) << "Found stale CID mapping " << vc.dcid << " -> " << it->second;
    return connection;
  }

  auto bootstrap_key = BootstrapRouteKey{.remote_address = msg_in.address, .routed_cid = vc.dcid};
  if (auto it = bootstrap_routes_.find(bootstrap_key); it != bootstrap_routes_.end()) {
    auto connection = find_connection(it->second);
    LOG_CHECK(connection) << "Found stale bootstrap route " << vc.dcid << " from " << msg_in.address << " -> "
                          << it->second;
    return connection;
  }

  auto r_initial_packet = VersionCid::from_initial_datagram(td::Slice(msg_in.storage));
  if (r_initial_packet.is_error()) {
    return reject(metrics::Reason::invalid, r_initial_packet.move_as_error());
  }
  auto initial_packet = r_initial_packet.move_as_ok();

  // Stage A — prove the source IP before spending any counted budget. A tokenless Initial draws
  // only a stateless Retry (or, under global overload, a silent drop); the retry token is bound to
  // the address, so a peer that echoes it back has proven its IP. Nothing per-IP is consulted or
  // consumed here — a spoofed src address cannot drain another peer's rate/discourage budget.
  // Whatever it answers statelessly (Retry / invalid-token close) it accounts for itself.
  auto initial_info = prepare_server_initial_info(initial_packet, msg_in.address);
  if (!initial_info.has_value()) {
    return std::shared_ptr<ConnectionState>{};
  }

  // Stage B — the IP is proven (valid retry token, or stateless retry disabled): now the per-IP
  // rate limiter, flood control, discourage throttle and per-IP half-open cap may spend budget.
  auto flood_addr = msg_in.address.get_ip_host();
  if (auto status = guard_.admit(flood_addr); status.is_error()) {
    return reject(metrics::Reason::limited, std::move(status));
  }

  auto r_state = create_inbound_connection(msg_in, initial_packet, *initial_info);
  if (r_state.is_error()) {
    return reject(metrics::Reason::internal, r_state.move_as_error());
  }
  auto state = r_state.move_as_ok();

  // Register the half-open connection (per-IP + FIFO count, live-per-IP count, discourage throttle
  // slot), start ingress shaping and evict the oldest half-open past the global cap. Shaping covers
  // the pre-handshake window; a trust upgrade unshapes it in on_handshake_completed.
  guard_.on_inbound_created(state->cid, flood_addr);

  return state;
}

td::Result<QuicConnectionId> QuicServer::connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key,
                                                 td::Slice alpn, td::Slice sni) {
  td::IPAddress remote_address;
  TRY_STATUS(remote_address.init_host_port(host.str(), port));
  TRY_RESULT(local_address, fd_.get_local_address());  // TODO: we may avoid system call here

  // Do not check flood here, because connect is initiated by us

  auto conn_options = build_connection_options();
  // Cap how many bidi streams the peer may open on a connection WE initiated. QuicSender pins it to
  // 0: an outbound connection is single-purpose (we open the query streams, the peer replies on
  // them), so a peer that tries to open one is refused at the transport layer, before any
  // reassembly or off-role delivery.
  if (options_.outbound_max_peer_streams_bidi.has_value()) {
    conn_options.max_streams_bidi = *options_.outbound_max_peer_streams_bidi;
  }
  if (options_.outbound_max_peer_streams_uni.has_value()) {
    conn_options.max_streams_uni = *options_.outbound_max_peer_streams_uni;
  }
  auto pimpl_callback = std::make_unique<PImplCallback>(*this, true);
  TRY_RESULT(p_impl, QuicConnectionPImpl::create_client(local_address, remote_address, std::move(client_key), alpn, sni,
                                                        std::move(pimpl_callback), conn_options));
  TRY_RESULT(state, install_connection(std::move(p_impl), remote_address, true, std::nullopt));
  guard_.on_outbound_created(state->cid);  // never pooled/shaped; egress cap stays log-only

  on_connection_updated(*state);
  return QuicConnectionId(state->cid);
}

// GSO/GRO hand the kernel one descriptor carrying several datagrams; the wire tier counts datagrams.
static td::uint64 datagram_count(const auto &message) {
  const size_t size = message.data.size(), gso_size = message.gso_size;
  return gso_size > 0 && size > gso_size ? (size + gso_size - 1) / gso_size : 1;
}

void QuicServer::reflect_socket_syscalls() {
  auto stats = fd_.get_syscall_stats();
  udp_wire_.dir.at(metrics::Direction::in).syscalls = metrics::Counter{stats.receive};
  udp_wire_.dir.at(metrics::Direction::out).syscalls = metrics::Counter{stats.send};
}

void QuicServer::record_egress(td::Span<td::UdpSocketFd::OutboundMessage> batch,
                               const td::UdpSocketFd::SendResult &result) {
  reflect_socket_syscalls();
  auto &egress = udp_wire_.dir.at(metrics::Direction::out);
  const bool mmsg = fd_.is_mmsg_enabled();
  for (const auto &message : batch.substr(0, result.sent)) {
    auto datagrams = datagram_count(message);
    egress.data.bytes.inc(message.data.size());
    egress.data.packets.inc(datagrams);
    batching_.egress_gso_segments.observe(static_cast<double>(datagrams));
    if (!mmsg) {
      batching_.egress_syscall_messages.observe(1);
    }
  }
  if (mmsg && result.sent > 0) {
    batching_.egress_syscall_messages.observe(static_cast<double>(result.sent));
  }
  // Consumed by the kernel, never put on the wire.
  for (const auto &message : batch.substr(result.sent, result.dropped)) {
    egress.dropped.at(metrics::Reason::internal).inc(datagram_count(message));
  }
}

void QuicServer::record_ingress(td::Span<td::UdpSocketFd::InboundMessage> batch) {
  reflect_socket_syscalls();
  auto &ingress = udp_wire_.dir.at(metrics::Direction::in);
  for (const auto &message : batch) {
    if (message.error->is_error()) {
      // Malformed datagram off the socket (e.g. truncated): a wire-level reject.
      ingress.dropped.at(metrics::Reason::invalid).inc();
      continue;
    }
    ingress.data.bytes.inc(message.data.size());
    ingress.data.packets.inc(datagram_count(message));
  }
}

void QuicServer::drain_ingress() {
  // The drain loop below always ends with a recvmmsg that returns nothing: that is how it learns to
  // stop, and it is what clears the fd's read flag. loop() also runs on timers and on send-side
  // wakeups, so draining unconditionally spent that empty syscall on every wakeup with no datagram
  // waiting. can_read() re-syncs the flag itself, and the flag is cleared only by the EAGAIN above,
  // so a skipped drain always has a wakeup behind it.
  if (!td::can_read(fd_)) {
    return;
  }
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
    // Counted before the early exit: a call that returns nothing still entered the kernel.
    record_ingress({ingress_messages_.data(), cnt});
    if (cnt == 0) {
      if (status.is_error()) {
        LOG_EVERY(ERROR) << "failed to drain incoming traffic: " << status;
      }
      break;
    }

    // Debug: log recvmmsg batch details periodically
    static std::atomic<size_t> ingress_log_counter = 0;
    if ((ingress_log_counter.fetch_add(1, std::memory_order_relaxed) & 0x3FFF) == 0) {
      FLOG(DEBUG) {
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
      };
    }

    for (size_t i = 0; i < cnt; i++) {
      bytes_budget -= std::max<size_t>(ingress_messages_[i].data.size(), 256);
      if (ingress_errors_[i].is_error()) {
        LOG(DEBUG) << "dropping inbound packet from " << ingress_msg_[i].address << ": " << ingress_errors_[i];
        continue;
      }
      ingress_msg_[i].storage = ingress_messages_[i].data;
      const size_t segment_size = ingress_messages_[i].gso_size;

      auto handle_packet = [&](UdpMessageBuffer &packet) {
        auto R = get_or_create_connection(packet);
        if (R.is_error()) {
          // A flood drops packets far faster than anything can read about them; the reject counters
          // carry the volume, this only has to keep a sample of the reason visible.
          LOG_EVERY(WARNING) << "dropping inbound packet from " << packet.address << ": " << R.error();
          return;
        }
        auto state = R.move_as_ok();
        if (!state) {
          return;
        }
        std::array<char, NGTCP2_MAX_UDP_PAYLOAD_SIZE> close_buffer;
        UdpMessageBuffer close_msg;
        close_msg.storage = td::MutableSlice(close_buffer.data(), close_buffer.size());
        auto handle_status = state->impl().handle_ingress(packet, close_msg);
        if (handle_status.is_ok()) {
          on_connection_updated(*state);
          // Queue/dequeue this connection in the shaper drain by its accrued debt (grants happen
          // only in the paced drain, so every grant is subject to the fair-share cap).
          guard_.on_ingress_debt(state->cid, state->impl().ingress_debt());
          return;
        }
        // Fatal errors are local/system faults (OOM, internal callback failure) worth a warning; a
        // deliberate CONNECTION_CLOSE carries the peer's close code and is worth INFO; everything
        // else is peer-induced (rejected/bad handshake, unknown SNI, protocol violation) and routine.
        if (ngtcp2_err_is_fatal(handle_status.code())) {
          record_transport_dropped(metrics::Direction::in, metrics::Reason::internal);
          LOG(WARNING) << "failed to handle ingress from " << *state << ": " << handle_status;
        } else if (handle_status.code() == NGTCP2_ERR_DRAINING) {
          LOG(INFO) << "closing connection after ingress from " << *state << ": " << handle_status;
        } else {
          state->impl().account_ingress_reject(transport_stats_);
          LOG(DEBUG) << "closing connection after ingress from " << *state << ": " << handle_status;
        }
        send_connection_close(*state, close_msg);  // no-op when empty
        on_connection_closed(state->cid);
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

  td::Span<td::UdpSocketFd::OutboundMessage> batch(egress_messages_.data() + pending_batch_sent_,
                                                   pending_batch_count_ - pending_batch_sent_);
  td::UdpSocketFd::SendResult result;
  auto status = fd_.send_messages(batch, result);
  record_egress(batch, result);

  pending_batch_sent_ += result.consumed();

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

size_t QuicServer::produce_next_egress(size_t first_message) {
  CHECK(first_message < kEgressBatch);
  const size_t max_packets = gso_enabled_ ? kMaxBurst : kEgressBatch - first_message;
  while (!active_trusted_.empty() || !active_untrusted_.empty()) {
    // Prefer trusted, but serve untrusted-first on the picker's anti-starvation pick; fall back to
    // the other class when the preferred one is empty (work-conserving).
    bool prefer_untrusted = picker_.prefer_untrusted();
    auto &primary = prefer_untrusted ? active_untrusted_ : active_trusted_;
    auto &fallback = prefer_untrusted ? active_trusted_ : active_untrusted_;
    auto &queue = primary.empty() ? fallback : primary;
    auto cid = queue.front();
    queue.pop_front();

    auto conn = find_connection(cid);
    if (!conn) {
      continue;  // stale entry
    }

    conn->in_active_queue = false;

    auto &batch = egress_batches_[first_message];
    auto &buffer = egress_buffers_[first_message];
    batch.storage = td::MutableSlice(buffer.data(), buffer.size());

    auto status = conn->impl().produce_egress(batch, max_packets);
    if (status.is_error()) {
      record_transport_dropped(metrics::Direction::out, metrics::Reason::internal);
      LOG(WARNING) << "produce_egress failed for " << conn->remote_address << ": " << status;
      continue;
    }
    if (batch.storage.empty()) {
      continue;  // no data, connection stays out of queue
    }
    picker_.on_served();  // advance only on a served pick, so skips don't skew the ratio
    if (!conn->peer_info.trusted && !conn->is_outbound) {
      guard_.note_activity(cid);  // egress keeps a long untrusted answer off the inactivity sweep
    }
    on_connection_updated(*conn);

    if (gso_enabled_) {
      egress_messages_[first_message] = {.to = &batch.address, .data = batch.storage, .gso_size = batch.gso_size};
      egress_message_owners_[first_message] = conn;
      return 1;
    }

    // The same pacing burst becomes ordinary UDP datagrams when UDP_SEGMENT is unavailable.
    CHECK(batch.gso_size != 0);
    size_t count = 0;
    for (size_t offset = 0; offset < batch.storage.size(); offset += batch.gso_size) {
      CHECK(first_message + count < kEgressBatch);
      const auto size = td::min(batch.gso_size, batch.storage.size() - offset);
      egress_messages_[first_message + count] = {
          .to = &batch.address, .data = batch.storage.substr(offset, size), .gso_size = 0};
      egress_message_owners_[first_message + count] = conn;
      ++count;
    }
    return count;
  }
  return 0;
}

void QuicServer::flush_egress() {
  td::PerfWarningTimer w("flush_egress_all", 0.1);

  auto packets_before = udp_wire_.dir.at(metrics::Direction::out).data.packets.value();
  SCOPE_EXIT {
    auto packets_after = udp_wire_.dir.at(metrics::Direction::out).data.packets.value();
    batching_.egress_flush_packets.observe(static_cast<double>(packets_after - packets_before));
  };

  // First flush any pending from previous call
  if (!flush_pending()) {
    return;  // still blocked
  }

  auto active_count = active_trusted_.size() + active_untrusted_.size();
  auto total_count = connections_.size();

  td::int64 bytes_budget = 10 << 20;
  while ((!active_trusted_.empty() || !active_untrusted_.empty()) && bytes_budget > 0) {
    size_t batch_count = 0;
    while (batch_count < kEgressBatch) {
      auto count = produce_next_egress(batch_count);
      if (count == 0) {
        break;
      }
      batch_count += count;
    }
    if (batch_count == 0) {
      break;
    }

    for (size_t i = 0; i < batch_count; i++) {
      bytes_budget -= std::max<size_t>(egress_messages_[i].data.size(), 256);
    }

    // Debug: log sendmmsg batch details (every 64 batches)
    static std::atomic<size_t> batch_log_counter = 0;
    if ((batch_log_counter.fetch_add(1, std::memory_order_relaxed) & 0x3FFF) == 0) {
      FLOG(DEBUG) {
        std::map<QuicConnectionId, size_t> conn_to_idx;
        std::vector<size_t> packet_conn_idx(batch_count);
        for (size_t i = 0; i < batch_count; i++) {
          auto [it, inserted] = conn_to_idx.try_emplace(egress_message_owners_[i]->cid, conn_to_idx.size());
          packet_conn_idx[i] = it->second;
        }
        sb << "sendmmsg batch=" << batch_count << " conns=" << conn_to_idx.size() << " [";
        for (size_t i = 0; i < batch_count; i++) {
          if (i > 0) {
            sb << ", ";
          }
          sb << egress_messages_[i].data.size();
          if (egress_messages_[i].gso_size > 0) {
            sb << "(gso=" << egress_messages_[i].gso_size << ")";
          }
          sb << "/c" << packet_conn_idx[i];
          if (gso_enabled_) {
            sb << "/s" << egress_message_owners_[i]->impl().get_last_packet_streams();
          }
        }
        sb << "] active/total=" << active_count << "/" << total_count;
      };
    }

    // Set pending and try to flush
    pending_batch_count_ = batch_count;
    pending_batch_sent_ = 0;
    if (!flush_pending()) {
      return;  // blocked, will continue on next wakeup
    }
  }
  if (bytes_budget <= 0) {
    yield();
  }
}

void QuicServer::update_alarm() {
  td::Timestamp alarm_ts = td::Timestamp::never();
  if (!timeout_heap_.empty()) {
    alarm_ts = td::Timestamp::at(timeout_heap_.top_key());
  }
  alarm_ts.relax(guard_.next_wakeup());
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
  bool existing = std::holds_alternative<QuicStreamID>(stream);
  auto state = find_connection(cid);
  if (!state) {
    if (existing) {
      guard_.on_stream_finished(cid, std::get<QuicStreamID>(stream));  // the app's last word on it
    }
    return td::Status::Error("Connection not found");
  }
  return send_stream_on(*state, std::move(stream), std::move(data), is_end);
}

td::Result<QuicStreamID> QuicServer::send_stream_on(ConnectionState &state,
                                                    std::variant<QuicStreamID, StreamOptions> stream,
                                                    td::BufferSlice data, bool is_end) {
  if (auto *existing = std::get_if<QuicStreamID>(&stream)) {
    auto sid = *existing;
    auto status = state.impl().buffer_stream(sid, std::move(data), is_end);
    if (status.is_error()) {
      // Error contract: a failed send leaves no live stream behind. Reset it so it can't pin a
      // max_streams credit or StreamState; resetting an already-gone stream is a no-op.
      state.impl().shutdown_stream(sid);
      guard_.on_ingress_debt(state.cid, state.impl().ingress_debt());
    }
    if (is_end || status.is_error()) {
      guard_.on_stream_finished(state.cid, sid);  // answered or reset: the app is done with the stream
    }
    on_connection_updated(state);
    TRY_STATUS(std::move(status));
    return sid;
  }

  auto &options = std::get<StreamOptions>(stream);
  TRY_RESULT(sid, state.impl().open_stream(options.direction, std::move(data), is_end));
  if (options.direction == StreamDirection::Bidirectional) {
    guard_.on_local_stream(state.cid, sid, options);
    if (options.max_size.has_value()) {
      state.impl().set_stream_receive_credit_from_max_size(sid, *options.max_size);
    }
  }
  on_connection_updated(state);
  return sid;
}

td::Result<QuicStreamID> QuicServer::send_message(QuicConnectionId cid, td::BufferSlice data) {
  auto state = find_connection(cid);
  if (!state) {
    return td::Status::Error("Connection not found");
  }
  if (state->impl().can_send_datagram(data.size())) {
    state->impl().send_datagram(std::move(data));
    on_connection_updated(*state);
    return -1;
  }
  // A peer that never advertised unidirectional stream credit would refuse every open forever, so
  // it gets the pre-unidirectional wire behaviour and its messages ride bidirectional streams.
  auto direction = !options_.message_streams_bidi && state->impl().peer_advertised_initial_uni_credit()
                       ? StreamDirection::Unidirectional
                       : StreamDirection::Bidirectional;
  auto mtu = state->peer_info.mtu;
  TRY_RESULT(sid, state->impl().open_stream(direction, std::move(data), true));
  if (direction == StreamDirection::Bidirectional) {
    // The peer answers on this same stream with an empty receipt; register the stream so the guard
    // knows the response bound instead of treating the receipt as an unsolicited inbound stream.
    guard_.on_local_stream(cid, sid, StreamOptions{.max_size = mtu});
  }
  on_connection_updated(*state);
  return sid;
}

}  // namespace ton::quic
