#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "auto/tl/ton_api.hpp"
#include "auto/tl/tonlib_api.h"
#include "td/actor/coro_utils.h"
#include "td/utils/as.h"

#include "quic-pimpl.h"
#include "quic-sender.h"

namespace ton::quic {

static td::Status connection_wait_timeout() {
  return td::Status::Error(ErrorCode::timeout, "timeout while waiting for a connection");
}

class QuicSender::ServerCallback final : public QuicServer::Callback {
 public:
  explicit ServerCallback(td::actor::ActorId<QuicSender> sender) : sender_(sender) {
  }

  td::Status on_connected(QuicConnectionId cid, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                          bool is_outbound, QuicServer::PeerMtuInfo peer_info) override {
    td::actor::send_closure(sender_, &QuicSender::on_connected, server_actor(), cid, local_id, peer_id, is_outbound,
                            peer_info);
    return td::Status::OK();
  }

  // The FloodGuard reassembles and flood-accounts; we forward each complete admitted message (or the
  // failure that ended the stream) straight onto the sender's stream-completion path.
  void on_message(QuicConnectionId cid, QuicStreamID sid, td::Result<td::BufferSlice> message) override {
    td::actor::send_closure(sender_, &QuicSender::on_stream_complete, server_actor(), cid, sid, std::move(message));
  }

  void on_closed(QuicConnectionId cid) override {
    td::actor::send_closure(sender_, &QuicSender::on_closed, cid);
  }
  void on_stream_closed(QuicConnectionId cid, StreamCloseEvent event) override {
    if (event.initiator == StreamInitiator::Peer) {
      if (event.direction == StreamDirection::Unidirectional) {
        // This lands behind the payload in QuicSender's mailbox; only then may the peer reuse the
        // stream credit.
        td::actor::send_closure(sender_, &QuicSender::return_peer_uni_stream_credit, server_actor(), cid);
      }
      return;
    }

    if (event.direction == StreamDirection::Unidirectional) {
      td::actor::send_closure(sender_, &QuicSender::on_local_uni_stream_closed, cid, event.sid, event.clean);
      return;
    }

    td::actor::send_closure(sender_, &QuicSender::on_local_bidi_stream_closed, cid, event.sid);
  }

 private:
  td::actor::ActorId<QuicServer> server_actor() {
    if (server_.empty()) {
      server_ = td::actor::actor_dynamic_cast<QuicServer>(td::actor::actor_id());
    }
    return server_;
  }

  td::actor::ActorId<QuicSender> sender_;
  td::actor::ActorId<QuicServer> server_;
};

td::Result<td::IPAddress> QuicSender::get_ip_address(const adnl::AdnlNode &node) {
  const adnl::AdnlAddressList &addr_list = node.addr_list();
  if (!addr_list.quic_addrs().empty()) {
    td::IPAddress ip = addr_list.quic_addrs()[0];
    LOG(DEBUG) << "Quic addr of " << node.compute_short_id() << " is " << ip.get_ip_str() << ":" << ip.get_port();
    return ip;
  }
  for (const auto &addr : addr_list.adnl_addrs()) {
    auto r_ip = addr->to_ip_address();
    if (r_ip.is_ok() && r_ip.ok().get_port() != 0) {
      td::IPAddress ip = r_ip.move_as_ok();
      ip.set_port((ip.get_port() + NODE_PORT_OFFSET) % 65536);
      LOG(DEBUG) << "Quic addr of " << node.compute_short_id() << " is " << ip.get_ip_str() << ":" << ip.get_port()
                 << " (computed from adnl addr)";
      return ip;
    }
  }
  LOG(DEBUG) << "No quic addr for " << node.compute_short_id();
  return td::Status::Error("no valid ip address");
}

QuicSender::QuicSender(td::actor::ActorId<adnl::AdnlPeerTable> adnl, td::actor::ActorId<keyring::Keyring> keyring,
                       QuicServer::Options options, FloodLimits flood_limits)
    : AdnlSenderEx(/* default_mtu = */ adnl::Adnl::get_mtu())
    , adnl_(std::move(adnl))
    , keyring_(std::move(keyring))
    , server_options_(options)
    , flood_limits_(flood_limits) {
  // Our outbound connections carry only the streams we open; the peer replies on them and never
  // opens its own. Advertise no peer-stream credit so the transport layer refuses any it tries.
  server_options_.outbound_max_peer_streams_bidi = 0;
  server_options_.outbound_max_peer_streams_uni = 0;
}

void QuicSender::send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) {
  send_message_coro(src, dst, std::move(data)).start_immediate().detach("quic:send_message");
}

void QuicSender::send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                            td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) {
  connect(std::move(promise),
          send_query_coro(src, dst, std::move(name), timeout, std::move(data), flood_limits_.default_max_answer_size));
}

void QuicSender::send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                               td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                               td::uint64 max_answer_size) {
  connect(std::move(promise), send_query_coro(src, dst, std::move(name), timeout, std::move(data), max_answer_size));
}

void QuicSender::get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                                 td::Promise<td::string> promise) {
  connect(std::move(promise), get_conn_ip_str_coro(l_id, p_id));
}

void QuicSender::set_quic_options(QuicServer::Options options) {
  server_options_ = options;
  server_options_.outbound_max_peer_streams_bidi = 0;  // see constructor
  server_options_.outbound_max_peer_streams_uni = 0;
}

void QuicSender::set_reserved_only(bool reserved_only) {
  reserved_only_ = reserved_only;
  for (auto &[port, server] : servers_by_port_) {
    td::actor::send_closure(server, &QuicServer::set_reserved_only, reserved_only);
  }
}

void QuicSender::add_id(adnl::AdnlNodeIdShort local_id) {
  add_local_id_coro(local_id).start().detach("add local id");
}

td::actor::Task<> QuicSender::add_id_and_wait(adnl::AdnlNodeIdShort local_id) {
  return add_local_id_coro(local_id);
}

void QuicSender::log_stats(std::string reason) {
  for (auto &it : servers_by_port_) {
    td::actor::send_closure(it.second.get(), &QuicServer::log_stats, reason);
  }
}

td::actor::Task<> QuicSender::collect(metrics::Context ctx) {
  std::vector<int> ports;
  std::vector<td::actor::StartedTask<ServerStats>> drains;
  for (const auto &[port, server] : servers_by_port_) {
    ports.push_back(port);
    drains.push_back(td::actor::ask(server, &QuicServer::collect));
  }
  auto stats = co_await td::actor::all_wrap(std::move(drains));

  ServerStats result;
  for (size_t i = 0; i < stats.size(); i++) {
    // A server that failed to answer folds its last answer instead of dropping out of the totals:
    // its counters go stale rather than backwards, and a rate over them stays readable.
    auto &last = last_server_stats_[ports[i]];
    if (stats[i].is_ok()) {
      last = stats[i].move_as_ok();
    }
    result += last;
  }

  metrics::Labeled<metrics::Gauge<td::uint64>, metrics::Direction, Trust> connections_ready;
  auto count_ready = [&](const auto &connections, metrics::Direction direction) {
    for (const auto &[_, connection] : connections) {
      if (!connection->is_ready) {
        continue;
      }
      connections_ready.at(direction, connection->trust).add(1);
    }
  };
  count_ready(inbound_, metrics::Direction::in);
  count_ready(outbound_, metrics::Direction::out);

  auto quic = ctx.with_name("quic");
  quic.collect(result);
  quic.with_name("transport").collect(connections_ready, "connections_ready");
  quic.collect(peer_metrics_);
  co_return {};
}

td::actor::Task<bool> QuicSender::has_peer_connection(adnl::AdnlNodeIdShort peer_id) {
  for (const auto &[cid, connection] : by_cid_) {
    if (connection->path.second == peer_id) {
      co_return true;
    }
  }
  co_return false;
}

void QuicSender::on_mtu_updated(td::optional<adnl::AdnlNodeIdShort> local_id,
                                td::optional<adnl::AdnlNodeIdShort> peer_id) {
  if (!local_id) {
    // No specific local id: refresh per-local-id default on every server that hosts it.
    for (auto &[id, server] : servers_by_id_) {
      td::actor::send_closure(server, &QuicServer::set_default_mtu, id, get_local_id_mtu(id));
    }
    return;
  }
  if (peer_id) {
    auto peer_mtu = get_peer_mtu_inner(local_id.value(), peer_id.value());
    auto trust = peer_mtu.trusted ? Trust::trusted : Trust::untrusted;
    auto update_trust = [&](auto &connections) {
      auto connection = connections.find({local_id.value(), peer_id.value()});
      if (connection != connections.end()) {
        connection->second->trust = trust;
      }
    };
    update_trust(inbound_);
    update_trust(outbound_);

    if (auto it = servers_by_id_.find(local_id.value()); it != servers_by_id_.end()) {
      td::actor::send_closure(it->second, &QuicServer::set_peer_mtu, local_id.value(), peer_id.value(), peer_mtu.mtu,
                              peer_mtu.trusted);
    }
    return;
  }
  if (auto it = servers_by_id_.find(local_id.value()); it != servers_by_id_.end()) {
    td::actor::send_closure(it->second, &QuicServer::set_default_mtu, local_id.value(),
                            get_local_id_mtu(local_id.value()));
  }
}

QuicSender::Connection::~Connection() {
  for (auto &[_, query] : responses) {
    query.promise.set_error(td::Status::Error("connection closed"));
  }
}

void QuicSender::start_up() {
  AdnlSenderInterface::start_up();
  next_dial_backoff_gc_ = td::Timestamp::in(kDialBackoffGcPeriod);
  update_waiter_alarm();
}

td::actor::Task<td::Unit> QuicSender::send_message_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                        td::BufferSlice data) {
  auto size = data.size();
  auto magic = metrics::resolve_tl_magic(data.as_slice());
  auto connection = get_or_create_connection({src, dst});
  auto trust = connection->trust;
  peer_metrics_.at(trust).app.record(metrics::Kind::message, metrics::Direction::out, magic, size);
  auto R = co_await send_message_coro_inner(std::move(connection), std::move(data), magic, trust).wrap();
  if (R.is_error()) {
    // Fire-and-forget path: nobody upstream sees this error, so account the drop here. The counter
    // carries the rate (a stream-credit storm drops every message for seconds); the log is throttled
    // to a breadcrumb.
    auto reason =
        R.error().code() == NGTCP2_ERR_STREAM_ID_BLOCKED ? metrics::Reason::limited : metrics::Reason::internal;
    peer_metrics_.at(trust).app.record_dropped(metrics::Direction::out, reason);
    static metrics::SlowLogThrottle throttle;
    if (throttle.take()) {
      LOG(INFO) << "Failed to send message: " << src << " -> " << dst << " size=" << size
                << " tl=" << metrics::tl_name(magic) << " " << R.error();
    }
  }
  co_return td::Unit{};
}

td::actor::Task<td::Unit> QuicSender::send_message_coro_inner(std::shared_ptr<Connection> connection,
                                                              td::BufferSlice data, td::int32 magic, Trust trust) {
  auto conn = co_await wait_connection_ready(std::move(connection));
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_message>(std::move(data));
  auto stream_id = co_await td::actor::ask(conn->server, &QuicServer::send_message, conn->cid, std::move(wire_data));
  if (stream_id < 0) {
    co_return td::Unit{};
  }
  td::Timer timer;
  // Nothing comes back on a unidirectional stream, so confirmation is the transport's ack of the data:
  // the stream closes once the peer has acknowledged all of it, and on_stream_closed records it.
  // The close cannot overtake this emplace: send_message only buffers the data and yields, so the
  // datagram leaves in a later QuicServer turn, while our resumption was already queued on this
  // actor when send_message returned. Both arrive here in FIFO order.
  CHECK(conn->messages.emplace(stream_id, Connection::PendingMessage{.magic = magic, .timer = timer, .trust = trust})
            .second);
  co_return td::Unit{};
}

td::actor::Task<td::BufferSlice> QuicSender::send_query_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                             std::string name, td::Timestamp timeout,
                                                             td::BufferSlice data, std::optional<td::uint64> limit) {
  if (timeout && timeout.is_in_past()) {
    co_return connection_wait_timeout();
  }
  auto magic = metrics::resolve_tl_magic(data.as_slice());
  auto conn = get_or_create_connection({src, dst});
  auto trust = conn->trust;
  peer_metrics_.at(trust).app.record(metrics::Kind::query, metrics::Direction::out, magic, data.size());
  // Connection setup is excluded from round-trip latency, but not from the caller's deadline.
  conn = co_await wait_connection_ready(std::move(conn), timeout);
  StreamOptions options{.max_size = limit, .deadline = timeout, .query_size = data.size(), .query_magic = magic};
  td::Timer timer;
  auto result = co_await send_query_coro_inner(std::move(conn), options, std::move(data), trust).wrap();
  peer_metrics_.at(trust).query_roundtrip.record(magic, dst, timer.elapsed(), result.is_ok());
  co_return std::move(result);
}

td::actor::Task<td::BufferSlice> QuicSender::send_query_coro_inner(std::shared_ptr<Connection> conn,
                                                                   StreamOptions options, td::BufferSlice data,
                                                                   Trust trust) {
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_query>(std::move(data));
  auto cid = conn->cid;
  auto server = conn->server;
  // create stream explicitly to avoid race with response
  auto stream_id = co_await td::actor::ask(server, &QuicServer::open_stream, cid, std::move(options));
  auto [future, answer_promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
  CHECK(
      conn->responses.emplace(stream_id, Connection::PendingQuery{.promise = std::move(answer_promise), .trust = trust})
          .second);
  conn = nullptr;  // don't keep connection, it may disconnect during our wait
  co_await td::actor::ask(server, &QuicServer::send_stream, cid, stream_id, std::move(wire_data), true);
  co_return co_await std::move(future);
}

td::actor::Task<std::string> QuicSender::get_conn_ip_str_coro(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id) {
  auto conn = co_await wait_connection_ready(get_or_create_connection({l_id, p_id}));
  co_return "<quic connection>";
}

td::actor::Task<> QuicSender::add_local_id_coro(adnl::AdnlNodeIdShort local_id) {
  adnl::AdnlNode node = co_await td::actor::ask(adnl_, &adnl::Adnl::get_self_node, local_id);
  auto port = (co_await get_ip_address(node)).get_port();
  auto priv_key = co_await td::actor::ask(keyring_, &keyring::Keyring::export_private_key, local_id.pubkey_hash());
  auto ed25519_key = co_await priv_key.export_as_ed25519();
  local_keys_.emplace(local_id, td::Ed25519::PrivateKey(ed25519_key.as_octet_string()));

  if (servers_by_id_.contains(local_id)) {
    LOG(DEBUG) << "Local id has already been added: " << local_id;
    co_return td::Unit{};
  }

  td::actor::ActorId<QuicServer> server;
  auto default_mtu = get_local_id_mtu(local_id);
  auto peers_mtu = get_local_id_peers_mtu(local_id);
  if (auto it = servers_by_port_.find(port); it != servers_by_port_.end()) {
    server = it->second.get();
    td::actor::send_closure(server, &QuicServer::set_default_mtu, local_id, default_mtu);
    for (const auto &[peer_id, peer_mtu] : peers_mtu) {
      td::actor::send_closure(server, &QuicServer::set_peer_mtu, local_id, peer_id, peer_mtu.mtu, peer_mtu.trusted);
    }
    td::actor::send_closure(server, &QuicServer::add_identity, local_id,
                            td::Ed25519::PrivateKey(local_keys_.at(local_id).as_octet_string()));
  } else {
    auto identity = ServerIdentity{.local_id = local_id,
                                   .key = td::Ed25519::PrivateKey(local_keys_.at(local_id).as_octet_string())};
    auto owned = co_await QuicServer::create(port, std::make_unique<ServerCallback>(actor_id(this)), default_mtu,
                                             std::move(identity), "ton", "0.0.0.0", server_options_, flood_limits_);
    server = owned.get();
    servers_by_port_[port] = std::move(owned);
    if (reserved_only_) {
      td::actor::send_closure(server, &QuicServer::set_reserved_only, true);
    }
    for (const auto &[peer_id, peer_mtu] : peers_mtu) {
      td::actor::send_closure(server, &QuicServer::set_peer_mtu, local_id, peer_id, peer_mtu.mtu, peer_mtu.trusted);
    }
  }
  servers_by_id_[local_id] = server;

  co_return td::Unit{};
}

std::shared_ptr<QuicSender::Connection> QuicSender::get_or_create_connection(AdnlPath path) {
  if (!waiter_timeouts_.empty() && waiter_timeouts_.top_key().is_in_past()) {
    alarm();
  }
  auto iter = outbound_.find(path);
  if (iter == outbound_.end()) {
    auto connection = std::make_shared<Connection>();
    connection->is_outbound = true;
    connection->path = path;
    connection->trust = peer_trust(path);
    CHECK(outbound_.emplace(path, connection).second);
    return connection;
  }
  return iter->second;
}

td::actor::Task<std::shared_ptr<QuicSender::Connection>> QuicSender::wait_connection_ready(
    std::shared_ptr<Connection> connection, td::Timestamp timeout) {
  if (timeout && timeout.is_in_past()) {
    if (!connection->init_started) {
      CHECK(outbound_.erase(connection->path) == 1);
    }
    co_return connection_wait_timeout();
  }
  if (connection->is_ready) {
    co_return connection;
  }
  bool start_init = false;
  if (!connection->init_started) {
    // No dial is in flight. Fail fast during backoff, but remove the placeholder so a later caller
    // can create a fresh connection once the window elapses.
    if (auto status = dial_backoff_.check(connection->path); status.is_error()) {
      auto it = outbound_.find(connection->path);
      if (it != outbound_.end() && it->second == connection) {
        outbound_.erase(it);
      }
      co_return std::move(status);
    }
    start_init = true;
  }
  auto [future, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
  auto &waiting = connection->waiting_ready;
  CHECK(waiting.size() <= Connection::MAX_WAITERS);
  td::Promise<td::Unit> dropped;
  if (waiting.size() == Connection::MAX_WAITERS) {
    dropped = take_waiter(waiting.front());
  }
  auto waiter = waiting.emplace(waiting.end(), waiting, std::move(promise));
  waiter->position = waiter;
  if (timeout) {
    waiter_timeouts_.insert(timeout, &*waiter);
  }
  update_waiter_alarm();

  if (start_init) {
    connection->init_started = true;
    init_connection(connection->path, connection).start().detach("init connection");
  }
  if (dropped) {
    // Promise callbacks run inline; defer so retries cannot recurse through evictions.
    td::actor::send_lambda_later(actor_id(this), [promise = std::move(dropped)]() mutable {
      promise.set_error(td::Status::Error("dropped waiting for a connection"));
    });
  }
  auto result = co_await std::move(future).wrap();
  if (timeout && timeout.is_in_past()) {
    co_return connection_wait_timeout();
  }
  co_await std::move(result);
  co_return connection;
}

td::Promise<td::Unit> QuicSender::take_waiter(Waiter &waiter) {
  if (waiter.in_heap()) {
    waiter_timeouts_.erase(&waiter);
  }
  CHECK(&*waiter.position == &waiter);
  auto promise = std::move(waiter.promise);
  waiter.queue.erase(waiter.position);
  return promise;
}

void QuicSender::update_waiter_alarm() {
  alarm_timestamp() = next_dial_backoff_gc_;
  if (!waiter_timeouts_.empty()) {
    alarm_timestamp().relax(waiter_timeouts_.top_key());
  }
}

td::actor::Task<td::Unit> QuicSender::init_connection(AdnlPath path, std::shared_ptr<Connection> connection) {
  auto result = co_await init_connection_inner(path, connection).wrap();
  if (result.is_error()) {
    // failed before any quic connection has been created
    LOG(WARNING) << "Failed to init connection: " << path << " " << result.error();
    dial_backoff_.on_failure(path, result.error());
    CHECK(outbound_.erase(path) == 1);
    finish_connection_init(connection, result.move_as_error());
  }
  // wait for on_connected
  co_return td::Unit{};
}

td::actor::Task<td::Unit> QuicSender::init_connection_inner(AdnlPath path, std::shared_ptr<Connection> conn) {
  auto node = co_await ask(adnl_, &adnl::Adnl::get_peer_node, path.first, path.second).trace("get_peer_node");

  auto peer_addr = co_await get_ip_address(node);
  auto peer_host = peer_addr.get_ip_host();
  auto peer_port = peer_addr.get_port();

  auto local_key_iter = local_keys_.find(path.first);
  if (local_key_iter == local_keys_.end()) {
    co_return td::Status::Error("no local key for source ADNL ID");
  }

  auto client_key = td::Ed25519::PrivateKey(local_key_iter->second.as_octet_string());

  auto server_iter = servers_by_id_.find(path.first);
  if (server_iter == servers_by_id_.end()) {
    co_return td::Status::Error("no QuicServer for local id");
  }

  auto server = server_iter->second;
  auto sni = ServerIdentity::sni(path.second);
  auto connection_id = co_await ask(server, &QuicServer::connect, peer_host, peer_port, std::move(client_key),
                                    td::Slice("ton"), td::Slice(sni))
                           .trace("connect");
  conn->cid = connection_id;
  conn->path = path;
  conn->server = server;
  CHECK(by_cid_.emplace(connection_id, conn).second);
  co_return td::Unit{};
}

void QuicSender::finish_connection_init(const std::shared_ptr<Connection> &connection, td::Result<td::Unit> result) {
  auto waiters = std::exchange(connection->waiting_ready, {});
  for (auto &waiter : waiters) {
    if (waiter.in_heap()) {
      waiter_timeouts_.erase(&waiter);
    }
  }
  update_waiter_alarm();
  for (auto &waiter : waiters) {
    waiter.promise.set_result(result.clone());
  }
}

void QuicSender::alarm() {
  std::vector<td::Promise<td::Unit>> expired;
  auto now = td::Timestamp::now();
  if (next_dial_backoff_gc_.is_in_past(now)) {
    dial_backoff_.gc();
    next_dial_backoff_gc_ = td::Timestamp::in(kDialBackoffGcPeriod);
  }
  while (!waiter_timeouts_.empty() && waiter_timeouts_.top_key().is_in_past(now)) {
    expired.push_back(take_waiter(*static_cast<Waiter *>(waiter_timeouts_.pop())));
  }
  update_waiter_alarm();
  for (auto &promise : expired) {
    promise.set_error(connection_wait_timeout());
  }
}

td::Result<td::Unit> QuicSender::on_connected_inner(td::actor::ActorId<QuicServer> server, QuicConnectionId cid,
                                                    adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                                    bool is_outbound, std::shared_ptr<Connection> &connection,
                                                    metrics::Reason &reject_reason) {
  reject_reason = metrics::Reason::invalid;
  if (auto it = by_cid_.find(cid); it != by_cid_.end()) {
    connection = it->second;
  }

  if (get_peer_mtu(local_id, peer_id) == 0) {
    reject_reason = metrics::Reason::limited;  // the path is administratively closed, not malformed
    return td::Status::Error(PSLICE() << "MTU 0 path [" << local_id << ';' << peer_id << ']');
  }

  auto path = AdnlPath{local_id, peer_id};
  if (connection) {
    if (connection->path != path) {
      return td::Status::Error(PSLICE() << "Key mismatch got:" << path << " expected " << connection->path);
    }
    CHECK(connection->is_outbound);
    CHECK(is_outbound);
    return td::Unit{};
  }

  if (is_outbound) {
    reject_reason = metrics::Reason::internal;  // we dialed it and then lost track of it
    return td::Status::Error("Unknown outbound connection");
  }

  // Close existing inbound connection for same path if any
  LOG_EVERY(ERROR) << "Create inbound " << path;
  if (auto old_it = inbound_.find(path); old_it != inbound_.end()) {
    auto old_conn = old_it->second;
    td::actor::send_closure(old_conn->server, &QuicServer::on_connection_closed, old_conn->cid);
    inbound_.erase(old_it);
  }
  connection = std::make_shared<Connection>();
  connection->server = server;
  connection->path = path;
  connection->trust = peer_trust(path);
  connection->cid = cid;
  connection->is_ready = true;
  connection->is_outbound = false;
  CHECK(by_cid_.emplace(cid, connection).second);
  inbound_[path] = connection;
  return td::Unit{};
}

void QuicSender::on_connected(td::actor::ActorId<QuicServer> server, QuicConnectionId cid,
                              adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id, bool is_outbound,
                              QuicServer::PeerMtuInfo peer_info) {
  std::shared_ptr<Connection> connection;
  auto reject_reason = metrics::Reason::invalid;
  auto result = on_connected_inner(server, cid, local_id, peer_id, is_outbound, connection, reject_reason);

  if (result.is_error()) {
    // ServerCallback::on_connected returned OK before hopping here, so this rejection is invisible to
    // QuicServer's own accounting; report it back. Disjoint from the callback's synchronous failures,
    // which never reach this actor.
    td::actor::send_closure(server, &QuicServer::record_handshake_reject, reject_reason, is_outbound);
    // the connection will be empty if an error happened during inbound connection initialization
    if (connection) {
      LOG(WARNING) << "Failed to init connection: " << connection->path << " " << result.error();
      connection->init_error = result.move_as_error();
    }
    td::actor::send_closure(server, &QuicServer::on_connection_closed, cid);
    return;
  }

  CHECK(connection);
  connection->trust = peer_info.trusted ? Trust::trusted : Trust::untrusted;
  connection->is_ready = true;
  td::actor::send_closure(server, &QuicServer::record_handshake_completed, is_outbound);
  if (is_outbound) {
    dial_backoff_.on_success(AdnlPath{local_id, peer_id});  // a successful dial clears the backoff
  }
  finish_connection_init(connection, td::Unit{});
}

void QuicSender::invalid_message(std::shared_ptr<Connection> connection, QuicStreamID stream_id, td::Slice data) {
  LOG_EVERY(ERROR) << "unexpected message on " << (connection->is_outbound ? "outbound" : "inbound")
                   << " connection CID:" << connection->cid << " SID:" << stream_id << " size:" << data.size()
                   << " tl:" << metrics::tl_name(data) << " head:" << td::format::as_hex_dump<4>(data.truncate(32));
}
void QuicSender::on_stream_complete_inbound(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                                            td::Result<td::BufferSlice> r_data) {
  CHECK(connection->is_ready);
  if (r_data.is_error()) {
    // No shutdown here: whoever failed the stream already reset it — the deadline path resets
    // directly, and a limit violation returns an error to the transport, which resets.
    LOG_EVERY(ERROR) << "Failed to read request stream: " << connection->path << " " << r_data.error();
  } else if (auto req_R = fetch_tl_object<ton_api::quic_Request>(r_data.ok().clone(), true); req_R.is_ok()) {
    auto request = req_R.move_as_ok();
    ton_api::downcast_call(*request, [&](auto &query) { on_request(connection, stream_id, query); });
  } else {
    invalid_message(connection, stream_id, r_data.ok());
    td::actor::send_closure(connection->server, &QuicServer::shutdown_stream, connection->cid, stream_id);
  }
}

void QuicSender::on_stream_complete_outbound(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                                             td::Result<td::BufferSlice> r_data) {
  if (!connection->is_ready) {
    LOG_EVERY(ERROR) << "drop stream from unauthenticated connection CID:" << connection->path << " SID:" << stream_id;
    return;
  }
  auto resp_it = connection->responses.find(stream_id);
  if (resp_it == connection->responses.end()) {
    // Legacy-bidi send_message streams register no response: the peer answers with an empty fin (or
    // the stream errors) and completes here with nothing pending. Not an error — the query path
    // registers a response entry, this one does not, so there is nothing to deliver. That empty fin
    // is the message's receipt, so it closes the confirmation entry here.
    record_message_confirmation(*connection, stream_id, r_data.is_ok());
    return;
  }

  if (r_data.is_error()) {
    auto error = r_data.move_as_error();
    LOG_EVERY(ERROR) << "Failed to read response stream: " << connection->path << " " << error;
    resp_it->second.promise.set_error(std::move(error));
  } else if (auto answer_R = fetch_tl_object<ton_api::quic_answer>(r_data.ok().clone(), true); answer_R.is_ok()) {
    auto answer = answer_R.move_as_ok();
    peer_metrics_.at(resp_it->second.trust)
        .app.record(metrics::Kind::answer, metrics::Direction::in, answer->data_.as_slice());
    resp_it->second.promise.set_value(std::move(answer->data_));
  } else {
    invalid_message(connection, stream_id, r_data.ok());
    resp_it->second.promise.set_error(td::Status::Error("Invalid answer"));
  }
  connection->responses.erase(resp_it);
  // No shutdown needed: the query was sent with fin and a complete answer means the peer fin'd
  // too, so the stream retires on its own (unlike the inbound garbage path above).
}

void QuicSender::on_stream_complete(td::actor::ActorId<QuicServer> server, QuicConnectionId cid, QuicStreamID stream_id,
                                    td::Result<td::BufferSlice> r_data) {
  // Never attribute peer data until the handshake identity has been accepted. A dropped stream is
  // still finished, or the guard would hold its in-flight charge forever.
  auto it = by_cid_.find(cid);
  if (it == by_cid_.end() || !it->second->is_ready || it->second->init_error) {
    LOG_EVERY(ERROR) << "drop stream from unknown or unauthenticated connection CID:" << cid << " SID:" << stream_id;
    if (stream_id >= 0) {
      td::actor::send_closure(server, &QuicServer::shutdown_stream, cid, stream_id);
    }
    return;
  }
  auto connection = it->second;

  // An RFC 9221 datagram (pseudo-sid -1) carries a fire-and-forget message and nothing else; it is
  // accepted only where requests are, on inbound connections.
  if (stream_id < 0) {
    if (connection->is_outbound || r_data.is_error()) {
      return;
    }
    auto message = fetch_tl_object<ton_api::quic_message>(r_data.move_as_ok(), true);
    if (message.is_ok()) {
      on_request(std::move(connection), stream_id, *message.ok());
    }
    return;
  }

  // Requests are accepted only on inbound connections, answers only on outbound ones: an outbound
  // connection carries our own queries, so a request arriving on it (or an answer on an inbound one)
  // is misdirected.
  if (connection->is_outbound) {
    on_stream_complete_outbound(connection, stream_id, std::move(r_data));
  } else {
    on_stream_complete_inbound(connection, stream_id, std::move(r_data));
  }
}

void QuicSender::return_peer_uni_stream_credit(td::actor::ActorId<QuicServer> server, QuicConnectionId cid) {
  td::actor::send_closure(server, &QuicServer::release_peer_uni_stream_credit, cid);
}

void QuicSender::on_local_uni_stream_closed(QuicConnectionId cid, QuicStreamID sid, bool clean) {
  auto it = by_cid_.find(cid);
  if (it == by_cid_.end()) {
    return;
  }
  // A local uni stream closes cleanly only after the peer transport acknowledged data and FIN.
  record_message_confirmation(*it->second, sid, clean);
}

void QuicSender::on_local_bidi_stream_closed(QuicConnectionId cid, QuicStreamID sid) {
  auto it = by_cid_.find(cid);
  if (it == by_cid_.end()) {
    return;
  }
  auto &connection = *it->second;

  // A legacy bidi message is confirmed by its empty receipt in on_stream_complete. If its stream
  // closes while the message is still pending, a clean transport close is not a substitute.
  record_message_confirmation(connection, sid, false);
  auto resp_it = connection.responses.find(sid);
  if (resp_it == connection.responses.end()) {
    return;
  }
  resp_it->second.promise.set_error(td::Status::Error("stream closed"));
  connection.responses.erase(resp_it);
}

void QuicSender::record_message_confirmation(Connection &connection, QuicStreamID stream_id, bool ok) {
  auto it = connection.messages.find(stream_id);
  if (it == connection.messages.end()) {
    return;
  }
  peer_metrics_.at(it->second.trust)
      .message_confirmation.record(it->second.magic, connection.path.second, it->second.timer.elapsed(), ok);
  connection.messages.erase(it);
}

void QuicSender::on_closed(QuicConnectionId cid) {
  auto it = by_cid_.find(cid);
  if (it == by_cid_.end()) {
    return;
  }
  auto connection = it->second;
  auto path = connection->path;

  by_cid_.erase(it);
  // Only erase from outbound_/inbound_ if cid matches (avoid race with newer connection)
  if (auto out_it = outbound_.find(path); out_it != outbound_.end() && out_it->second->cid == cid) {
    outbound_.erase(out_it);
  }
  if (auto in_it = inbound_.find(path); in_it != inbound_.end() && in_it->second->cid == cid) {
    inbound_.erase(in_it);
  }

  // Nothing will confirm the messages still in flight on this connection.
  while (!connection->messages.empty()) {
    record_message_confirmation(*connection, connection->messages.begin()->first, false);
  }

  auto status = std::move(connection->init_error).value_or(td::Status::Error("connection closed"));
  // An outbound connection that closed before it ever became ready is a failed dial (handshake
  // failure or a peer close mid-handshake) — back it off. A ready connection closing later is normal.
  if (connection->is_outbound && !connection->is_ready) {
    dial_backoff_.on_failure(path, status);
  }
  finish_connection_init(connection, std::move(status));
}

void QuicSender::on_request(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                            ton_api::quic_query &query) {
  on_inbound_query(connection, stream_id, std::move(query.data_)).start_immediate().detach();
}

void QuicSender::on_request(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                            ton_api::quic_message &message) {
  peer_metrics_.at(connection->trust)
      .app.record(metrics::Kind::message, metrics::Direction::in, message.data_.as_slice());
  on_inbound_message(connection, stream_id, std::move(message.data_)).start_immediate().detach();
}

td::actor::Task<> QuicSender::on_inbound_message(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                                                 td::BufferSlice data) {
  // Hold the stream (and its in-flight charge) until the message is actually delivered, not just
  // enqueued — otherwise the receipt below would release the charge before adnl handled the message,
  // and a peer could pump messages past the in-flight budget.
  auto delivered = co_await td::actor::ask(adnl_, &adnl::AdnlPeerTable::deliver_ex, connection->path.second,
                                           connection->path.first, std::move(data))
                       .wrap();
  LOG_IF(INFO, delivered.is_error()) << "inbound message cid=" << connection->cid << " sid=" << stream_id << ": "
                                     << delivered.error();
  if (stream_id < 0) {
    co_return td::Unit{};  // a datagram held no stream and no in-flight charge
  }
  if (!ngtcp2_is_bidi_stream(stream_id)) {
    // A unidirectional stream is already closed at the transport (its credit returned via
    // on_stream_closed, queued behind the payload). This is the app's last word on it, which
    // releases the guard's in-flight charge; the transport-level reset inside is a no-op.
    td::actor::send_closure(connection->server, &QuicServer::shutdown_stream, connection->cid, stream_id);
    co_return td::Unit{};
  }
  // A message on a bidirectional stream (an old peer, or message_streams_bidi) has a response half:
  // the empty fin is its delivery receipt, and sending it only now — after the message was
  // delivered — is what makes the peer's budget real backpressure.
  auto sent = co_await td::actor::ask(connection->server, &QuicServer::send_stream, connection->cid,
                                      std::variant<QuicStreamID, StreamOptions>{stream_id}, td::BufferSlice{}, true)
                  .wrap();
  LOG_IF(INFO, sent.is_error()) << "inbound message fin cid=" << connection->cid << " sid=" << stream_id << ": "
                                << sent.error();
  co_return td::Unit{};
}

td::actor::Task<> QuicSender::on_inbound_query(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                                               td::BufferSlice query) {
  auto trust = connection->trust;
  peer_metrics_.at(trust).app.record(metrics::Kind::query, metrics::Direction::in, query.as_slice());
  // Admission already happened: the query was charged to the server's in-flight pool at dispatch,
  // and the stream holds that charge until it closes — which this handler arranges below, either way.
  auto answer = co_await td::actor::ask(adnl_, &adnl::AdnlPeerTable::deliver_query, connection->path.second,
                                        connection->path.first, std::move(query))
                    .wrap();

  // Exactly one thing finishes the stream: the answer (send_stream resets it itself on failure), or
  // the reset here when the query was refused or failed — so the peer fails fast instead of leaking
  // the stream until the pending-stream deadline. All failures are expected: log, don't assert.
  if (answer.is_error()) {
    LOG(INFO) << "inbound query cid=" << connection->cid << " sid=" << stream_id << ": " << answer.error();
    td::actor::send_closure(connection->server, &QuicServer::shutdown_stream, connection->cid, stream_id);
    co_return td::Unit{};
  }
  auto data = answer.move_as_ok();
  peer_metrics_.at(trust).app.record(metrics::Kind::answer, metrics::Direction::out, data.as_slice());
  auto sent = co_await td::actor::ask(connection->server, &QuicServer::send_stream, connection->cid,
                                      std::variant<QuicStreamID, StreamOptions>{stream_id},
                                      create_serialize_tl_object<ton_api::quic_answer>(std::move(data)), true)
                  .wrap();
  LOG_IF(INFO, sent.is_error()) << "inbound answer cid=" << connection->cid << " sid=" << stream_id << ": "
                                << sent.error();
  co_return td::Unit{};
}

Trust QuicSender::peer_trust(const AdnlPath &path) {
  return get_peer_mtu_inner(path.first, path.second).trusted ? Trust::trusted : Trust::untrusted;
}

}  // namespace ton::quic
