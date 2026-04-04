#include <atomic>
#include <chrono>

#include "td/actor/actor.h"
#include "td/utils/Timer.h"

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

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::create(int port, td::Ed25519::PrivateKey server_key,
                                                               std::unique_ptr<Callback> callback,
                                                               td::uint64 default_mtu, td::Slice alpn,
                                                               td::Slice bind_host) {
  return create(port, std::move(server_key), std::move(callback), default_mtu, alpn, bind_host, Options{}, {});
}

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::create(int port, td::Ed25519::PrivateKey server_key,
                                                               std::unique_ptr<Callback> callback,
                                                               td::uint64 default_mtu, td::Slice alpn,
                                                               td::Slice bind_host, Options options,
                                                               std::map<adnl::AdnlNodeIdShort, td::uint64> peers_mtu) {
  CHECK(callback);
  td::IPAddress local_addr;
  TRY_STATUS(local_addr.init_host_port(bind_host.str(), port));

  TRY_RESULT(fd, td::UdpSocketFd::open(local_addr));

  auto name = PSTRING() << "QUIC:" << local_addr;
  return td::actor::create_actor<QuicServer>(td::actor::ActorOptions().with_name(name).with_poll(true), std::move(fd),
                                             std::move(server_key), default_mtu, td::BufferSlice(alpn),
                                             std::move(callback), options, std::move(peers_mtu));
}

QuicServer::QuicServer(td::UdpSocketFd fd, td::Ed25519::PrivateKey server_key, td::uint64 default_mtu,
                       td::BufferSlice alpn, std::unique_ptr<Callback> callback, Options options,
                       std::map<adnl::AdnlNodeIdShort, td::uint64> peers_mtu)
    : fd_(std::move(fd))
    , alpn_(std::move(alpn))
    , server_key_(std::move(server_key))
    , options_(options)
    , conn_rate_limiters_(options.new_connection_rate_limit_capacity, options.new_connection_rate_limit_period)
    , global_conn_rate_limiter_(options.global_new_connection_rate_limit_capacity,
                                options.global_new_connection_rate_limit_period)
    , gso_enabled_(options.enable_gso && td::UdpSocketFd::is_gso_supported())
    , callback_(std::move(callback))
    , default_mtu_(default_mtu)
    , peers_mtu_(std::move(peers_mtu)) {
  td::Random::secure_bytes(td::MutableSlice(retry_secret_.data(), retry_secret_.size()));
  callback_->set_peer_mtu_callback([this](adnl::AdnlNodeIdShort peer_id) {
    td::uint64 mtu = default_mtu_;
    auto it = peers_mtu_.find(peer_id);
    if (it != peers_mtu_.end()) {
      mtu = std::max(mtu, it->second);
    }
    return mtu;
  });
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

td::Status QuicServer::ensure_flood_allowed(const std::string &flood_addr) {
  if (!options_.flood_control.has_value()) {
    return td::Status::OK();
  }
  if (auto it = flood_map_.find(flood_addr); it != flood_map_.end() && it->second >= *options_.flood_control) {
    return td::Status::Error("flood control overflow");
  }
  TRY_STATUS(conn_rate_limiters_.take_new_connection(flood_addr));
  if (!global_conn_rate_limiter_.take()) {
    return td::Status::Error("global new connection rate limit exceeded");
  }
  return td::Status::OK();
}

void QuicServer::flood_on_inbound_connection_created(const std::string &flood_addr) {
  if (!options_.flood_control.has_value()) {
    return;
  }
  flood_map_[flood_addr]++;
}

void QuicServer::flood_on_inbound_connection_closed(const std::string &flood_addr) {
  if (!options_.flood_control.has_value()) {
    return;
  }
  auto it = flood_map_.find(flood_addr);
  if (it == flood_map_.end()) {
    return;
  }
  if (--it->second == 0) {
    flood_map_.erase(it);
  }
}

QuicConnectionOptions QuicServer::build_connection_options() const {
  QuicConnectionOptions conn_options;
  conn_options.cc_algo = options_.cc_algo;
  if (options_.max_streams_bidi.has_value()) {
    conn_options.max_streams_bidi = *options_.max_streams_bidi;
  }
  return conn_options;
}

void QuicServer::on_local_cid_issued(const QuicConnectionId &primary_cid, const QuicConnectionId &cid) {
  bind_cid(primary_cid, cid);
}

void QuicServer::on_local_cid_retired(const QuicConnectionId &primary_cid, const QuicConnectionId &cid) {
  unbind_cid(primary_cid, cid);
}

td::Result<std::optional<ServerInitialInfo>> QuicServer::prepare_server_initial_info(
    const VersionCid &initial_packet, const td::IPAddress &remote_address) {
  ServerInitialInfo initial_info{
      .packet = initial_packet,
      .original_dcid = initial_packet.dcid,
      .retry_scid = std::nullopt,
  };

  if (!options_.stateless_retry) {
    return std::optional<ServerInitialInfo>(std::move(initial_info));
  }

  if (initial_packet.token.empty()) {
    TRY_STATUS(send_retry(initial_packet, remote_address));
    return std::optional<ServerInitialInfo>{};
  }

  auto original_dcid = verify_retry_token(initial_packet, remote_address);
  if (original_dcid.is_error()) {
    LOG(DEBUG) << "invalid Retry token from " << remote_address << ": " << original_dcid.error();
    TRY_STATUS(send_invalid_token_connection_close(initial_packet, remote_address));
    return std::optional<ServerInitialInfo>{};
  }

  initial_info.original_dcid = original_dcid.move_as_ok();
  initial_info.retry_scid = initial_packet.dcid;
  return std::optional<ServerInitialInfo>(std::move(initial_info));
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

td::Status QuicServer::send_stateless_datagram(td::Slice packet_kind, const td::IPAddress &remote_address,
                                               td::Slice data) {
  td::UdpSocketFd::OutboundMessage message{.to = &remote_address, .data = data, .gso_size = 0};
  bool is_sent = false;
  auto status = fd_.send_message(message, is_sent);
  egress_stats_.syscalls++;
  if (is_sent) {
    egress_stats_.packets++;
    egress_stats_.bytes += data.size();
  }
  if (status.is_error()) {
    return status;
  }
  if (!is_sent) {
    LOG(DEBUG) << "dropping stateless " << packet_kind << " to " << remote_address << ": send_message blocked";
    return td::Status::OK();
  }
  return td::Status::OK();
}

td::Status QuicServer::send_retry(const VersionCid &packet, const td::IPAddress &remote_address) {
  auto client_scid = QuicConnectionIdAccess::to_ngtcp2(packet.scid);
  auto original_dcid = QuicConnectionIdAccess::to_ngtcp2(packet.dcid);
  auto retry_scid = QuicConnectionIdAccess::to_ngtcp2(QuicConnectionId::random());

  std::array<uint8_t, NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2> token;
  auto tokenlen = ngtcp2_crypto_generate_retry_token2(
      token.data(), retry_secret_.data(), retry_secret_.size(), packet.version,
      reinterpret_cast<const ngtcp2_sockaddr *>(remote_address.get_sockaddr()),
      static_cast<ngtcp2_socklen>(remote_address.get_sockaddr_len()), &retry_scid, &original_dcid, retry_token_now());
  if (tokenlen < 0) {
    return td::Status::Error("failed to generate retry token");
  }

  std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> datagram;
  auto datagram_size = ngtcp2_crypto_write_retry(datagram.data(), datagram.size(), packet.version, &client_scid,
                                                 &retry_scid, &original_dcid, token.data(), tokenlen);
  if (datagram_size < 0) {
    return td::Status::Error("failed to write retry packet");
  }

  LOG(DEBUG) << "sending Retry to " << remote_address << " for original dcid=" << packet.dcid;
  return send_stateless_datagram(
      "Retry", remote_address,
      td::Slice(reinterpret_cast<const char *>(datagram.data()), static_cast<size_t>(datagram_size)));
}

td::Status QuicServer::send_invalid_token_connection_close(const VersionCid &packet,
                                                           const td::IPAddress &remote_address) {
  auto client_scid = QuicConnectionIdAccess::to_ngtcp2(packet.scid);
  auto original_dcid = QuicConnectionIdAccess::to_ngtcp2(packet.dcid);

  std::array<uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> datagram;
  auto datagram_size = ngtcp2_crypto_write_connection_close(
      datagram.data(), datagram.size(), packet.version, &client_scid, &original_dcid, NGTCP2_INVALID_TOKEN, nullptr, 0);
  if (datagram_size < 0) {
    return td::Status::Error("failed to write stateless connection close");
  }

  LOG(DEBUG) << "sending invalid-token connection close to " << remote_address;
  return send_stateless_datagram(
      "invalid-token connection close", remote_address,
      td::Slice(reinterpret_cast<const char *>(datagram.data()), static_cast<size_t>(datagram_size)));
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

void QuicServer::collect_stats(td::Promise<Stats> P) {
  Stats stats;
  for (auto &[id, conn] : connections_) {
    Stats::Entry entry{.total_conns = 1, .impl_stats = conn->impl_->get_stats()};
    stats.summary = stats.summary + entry;
    stats.per_conn[id] = entry;
  }
  return P.set_value(std::move(stats));
}

void QuicServer::on_connection_closed(QuicConnectionId cid) {
  auto it = connections_.find(cid);
  if (it == connections_.end()) {
    LOG(WARNING) << "Can't find connection for closing " << cid;
    return;
  }
  auto state = it->second;
  LOG(INFO) << "Close connection: " << *state;
  unbind_all_cids(*state);
  if (state->in_heap()) {
    timeout_heap_.erase(state.get());
  }
  if (!state->is_outbound) {
    flood_on_inbound_connection_closed(state->remote_address.get_ip_host());
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

  {
    td::PerfWarningTimer w("cleanup_conn_rate_limiters", 0.1);
    conn_rate_limiters_.cleanup();
  }
}

void QuicServer::erase_pending_connections() {
  for (auto cid : to_erase_connections_) {
    on_connection_closed(cid);
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

void QuicServer::set_default_mtu(td::uint64 mtu) {
  default_mtu_ = mtu;
}

void QuicServer::set_peer_mtu(adnl::AdnlNodeIdShort peer_id, td::uint64 mtu) {
  if (mtu == 0) {
    peers_mtu_.erase(peer_id);
  } else {
    peers_mtu_[peer_id] = mtu;
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
    auto status = callback_.on_connected(cid_, std::move(event.peer_public_key), is_outbound_);
    if (status.is_error()) {
      LOG(WARNING) << "on_connected failed for " << cid_ << ": " << status;
      server_.to_erase_connections_.push_back(cid_);
    }
  }

  td::Status on_stream_data(StreamDataEvent event) override {
    return callback_.on_stream(cid_, event.sid, std::move(event.data), event.fin);
  }
  void on_stream_closed(QuicStreamID sid) override {
    return callback_.on_stream_closed(cid_, sid);
  }

 private:
  QuicServer &server_;
  QuicServer::Callback &callback_;
  QuicConnectionId cid_;
  bool is_outbound_;
};

td::Result<std::shared_ptr<QuicServer::ConnectionState>> QuicServer::get_or_create_connection(
    const UdpMessageBuffer &msg_in) {
  TRY_RESULT(vc, VersionCid::from_datagram(td::Slice(msg_in.storage)));

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

  TRY_RESULT(initial_packet, VersionCid::from_initial_datagram(td::Slice(msg_in.storage)));

  auto flood_addr = msg_in.address.get_ip_host();
  TRY_STATUS(ensure_flood_allowed(flood_addr));

  TRY_RESULT(initial_info, prepare_server_initial_info(initial_packet, msg_in.address));
  if (!initial_info.has_value()) {
    return std::shared_ptr<ConnectionState>{};
  }

  // Create new connection to handle unknown inbound message
  TRY_RESULT(local_address, fd_.get_local_address());

  auto conn_options = build_connection_options();
  auto pimpl_callback = std::make_unique<PImplCallback>(*this, false);
  TRY_RESULT(p_impl, QuicConnectionPImpl::create_server(local_address, msg_in.address, server_key_, alpn_.as_slice(),
                                                        *initial_info, std::move(pimpl_callback), conn_options));
  TRY_RESULT(state, install_connection(std::move(p_impl), msg_in.address, false, initial_packet.dcid));

  flood_on_inbound_connection_created(flood_addr);

  return state;
}

td::Result<QuicConnectionId> QuicServer::connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key,
                                                 td::Slice alpn) {
  td::IPAddress remote_address;
  TRY_STATUS(remote_address.init_host_port(host.str(), port));
  TRY_RESULT(local_address, fd_.get_local_address());  // TODO: we may avoid system call here

  // Do not check flood here, because connect is initiated by us

  auto conn_options = build_connection_options();
  auto pimpl_callback = std::make_unique<PImplCallback>(*this, true);
  TRY_RESULT(p_impl, QuicConnectionPImpl::create_client(local_address, remote_address, std::move(client_key), alpn,
                                                        std::move(pimpl_callback), conn_options));
  TRY_RESULT(state, install_connection(std::move(p_impl), remote_address, true, std::nullopt));

  on_connection_updated(*state);
  return QuicConnectionId(state->cid);
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
      LOG(DEBUG) << sb.as_cslice();
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
        if (!state) {
          return;
        }
        if (auto handle_status = state->impl().handle_ingress(packet); handle_status.is_error()) {
          LOG(WARNING) << "failed to handle ingress from " << *state << ":  " << handle_status;
          on_connection_closed(state->cid);  // TODO: probably we have to tell here to quic that connection is closed
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
      LOG(DEBUG) << sb.as_cslice();
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
  alarm_ts.relax(conn_rate_limiters_.next_cleanup_at());
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
    auto &options = std::get<StreamOptions>(stream);
    callback_->set_stream_options(cid, sid, options);
    if (options.max_size.has_value()) {
      state->impl().set_stream_receive_credit_from_max_size(sid, *options.max_size);
    }
  }

  TRY_STATUS(state->impl().buffer_stream(sid, std::move(data), is_end));
  on_connection_updated(*state);
  return sid;
}

}  // namespace ton::quic
