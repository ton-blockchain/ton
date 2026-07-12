#include <algorithm>
#include <optional>
#include <utility>

#include "auto/tl/ton_api.hpp"
#include "auto/tl/tonlib_api.h"
#include "td/actor/coro_utils.h"
#include "td/utils/as.h"

#include "quic-pimpl.h"
#include "quic-sender.h"

namespace ton::quic {

static td::uint32 get_magic(const td::Slice data) {
  return data.size() >= 4 ? td::as<td::uint32>(data.data()) : 0;
}
static td::uint32 get_magic(const td::BufferSlice &data) {
  return get_magic(data.as_slice());
}

class QuicSender::ServerCallback final : public QuicServer::Callback {
 public:
  explicit ServerCallback(td::actor::ActorId<QuicSender> sender) : sender_(sender) {
  }

  td::Status on_connected(QuicConnectionId cid, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                          bool is_outbound, QuicServer::PeerMtuInfo peer_info) override {
    auto server = td::actor::actor_dynamic_cast<QuicServer>(td::actor::actor_id());
    CHECK(!server.empty());
    td::actor::send_closure(sender_, &QuicSender::on_connected, server, cid, local_id, peer_id, is_outbound, peer_info);
    return td::Status::OK();
  }

  // The FloodGuard reassembles and flood-accounts; we forward each complete admitted message (or the
  // failure that ended the stream) straight onto the sender's stream-completion path.
  void on_message(QuicConnectionId cid, QuicStreamID sid, td::Result<td::BufferSlice> message) override {
    td::actor::send_closure(sender_, &QuicSender::on_stream_complete, cid, sid, std::move(message));
  }

  void on_closed(QuicConnectionId cid) override {
    td::actor::send_closure(sender_, &QuicSender::on_closed, cid);
  }
  void on_stream_closed(QuicConnectionId cid, QuicStreamID sid) override {
    td::actor::send_closure(sender_, &QuicSender::on_stream_closed, cid, sid);
  }

 private:
  td::actor::ActorId<QuicSender> sender_;
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
  // opens its own. Advertise no bidi-stream credit so the transport layer refuses any it tries.
  server_options_.outbound_max_peer_streams_bidi = 0;
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

void QuicSender::log_stats(std::string reason) {
  for (auto &it : servers_by_port_) {
    td::actor::send_closure(it.second.get(), &QuicServer::log_stats, reason);
  }
}

td::actor::Task<QuicSender::Stats> QuicSender::collect_stats() {
  Stats stats;
  for (auto &[_, server] : servers_by_port_) {
    auto serv_stats = co_await td::actor::ask(server, &QuicServer::collect_stats);
    stats.summary = stats.summary + Stats::Entry{.server_stats = serv_stats.summary};
    for (auto &[id, conn_stats] : serv_stats.per_conn) {
      if (!by_cid_.contains(id))
        continue;
      stats.per_path[by_cid_[id]->path] = Stats::Entry{.server_stats = conn_stats};
    }
  }
  co_return stats;
}

td::actor::Task<> QuicSender::collect(metrics::Context ctx) {
  // TODO
  co_return {};
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
  auto it = servers_by_id_.find(local_id.value());
  if (it == servers_by_id_.end()) {
    return;
  }
  if (!peer_id) {
    td::actor::send_closure(it->second, &QuicServer::set_default_mtu, local_id.value(),
                            get_local_id_mtu(local_id.value()));
    return;
  }
  auto peer_mtu = get_peer_mtu_inner(local_id.value(), peer_id.value());
  td::actor::send_closure(it->second, &QuicServer::set_peer_mtu, local_id.value(), peer_id.value(), peer_mtu.mtu,
                          peer_mtu.trusted);
}

QuicSender::Connection::~Connection() {
  for (auto &[_, P] : responses) {
    P.set_error(td::Status::Error("connection closed"));
  }
}

void QuicSender::start_up() {
  AdnlSenderInterface::start_up();
  alarm_timestamp() = td::Timestamp::in(kDialBackoffGcPeriod);
}

void QuicSender::alarm() {
  dial_backoff_.gc();
  alarm_timestamp() = td::Timestamp::in(kDialBackoffGcPeriod);
}

td::actor::Task<td::Unit> QuicSender::send_message_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                        td::BufferSlice data) {
  auto size = data.size();
  auto magic = get_magic(data);
  auto R = co_await send_message_coro_inner(src, dst, std::move(data)).wrap();
  LOG_IF(INFO, R.is_error()) << "Failed to send message: " << src << " -> " << dst << " size=" << size
                             << " magic=" << td::format::as_hex(magic) << " " << R.error();
  co_return td::Unit{};
}

td::actor::Task<td::Unit> QuicSender::send_message_coro_inner(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                              td::BufferSlice data) {
  auto conn = co_await find_or_create_connection({src, dst});
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_message>(std::move(data));
  co_await td::actor::ask(conn->server, &QuicServer::send_stream, conn->cid, StreamOptions{get_peer_mtu(src, dst)},
                          std::move(wire_data), true);
  co_return td::Unit{};
}

td::actor::Task<td::BufferSlice> QuicSender::send_query_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                             std::string name, td::Timestamp timeout,
                                                             td::BufferSlice data, std::optional<td::uint64> limit) {
  auto conn = co_await find_or_create_connection({src, dst});
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_query>(std::move(data));
  auto cid = conn->cid;
  auto server = conn->server;
  // create stream explicitly to avoid race with response
  auto stream_id = co_await td::actor::ask(server, &QuicServer::open_stream, cid,
                                           StreamOptions{.max_size = limit, .deadline = timeout});
  auto [future, answer_promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
  CHECK(conn->responses.emplace(stream_id, std::move(answer_promise)).second);
  conn = nullptr;  // don't keep connection, it may disconnect during our wait
  co_await td::actor::ask(server, &QuicServer::send_stream, cid, stream_id, std::move(wire_data), true);
  co_return co_await std::move(future);
}

td::actor::Task<std::string> QuicSender::get_conn_ip_str_coro(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id) {
  auto conn = co_await find_or_create_connection({l_id, p_id});
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

td::actor::Task<std::shared_ptr<QuicSender::Connection>> QuicSender::find_or_create_connection(AdnlPath path) {
  std::shared_ptr<Connection> connection;
  auto iter = outbound_.find(path);
  if (iter == outbound_.end()) {
    // No connection and no dial in flight. If the peer is in dial backoff, fail fast instead of
    // starting a new dial (and instead of making the query wait out the backoff) — the next query
    // after the backoff window elapses will trigger a fresh dial.
    if (auto status = dial_backoff_.check(path); status.is_error()) {
      co_return std::move(status);
    }
    connection = std::make_shared<Connection>();
    connection->is_outbound = true;
    CHECK(outbound_.emplace(path, connection).second);
  } else {
    connection = iter->second;
  }

  if (connection->is_ready) {
    co_return connection;
  }
  auto [future, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
  connection->waiting_ready.push_back(std::move(promise));

  if (!connection->init_started) {
    connection->init_started = true;
    init_connection(path, connection).start().detach("init connection");
  }

  co_await std::move(future);

  co_return connection;
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
  auto promises = std::move(connection->waiting_ready);
  for (auto &promise : promises) {
    promise.set_result(result.clone());
  }
}

td::Result<td::Unit> QuicSender::on_connected_inner(td::actor::ActorId<QuicServer> server, QuicConnectionId cid,
                                                    adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                                    bool is_outbound, std::shared_ptr<Connection> &connection) {
  if (auto it = by_cid_.find(cid); it != by_cid_.end()) {
    connection = it->second;
  }

  if (get_peer_mtu(local_id, peer_id) == 0) {
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
  auto result = on_connected_inner(server, cid, local_id, peer_id, is_outbound, connection);

  if (result.is_error()) {
    // the connection will be empty if an error happened during inbound connection initialization
    if (connection) {
      LOG(WARNING) << "Failed to init connection: " << connection->path << " " << result.error();
      connection->init_error = result.move_as_error();
    }
    td::actor::send_closure(server, &QuicServer::on_connection_closed, cid);
    return;
  }

  CHECK(connection);
  connection->is_ready = true;
  if (is_outbound) {
    dial_backoff_.on_success(AdnlPath{local_id, peer_id});  // a successful dial clears the backoff
  }
  finish_connection_init(connection, td::Unit{});
}

void QuicSender::invalid_message(std::shared_ptr<Connection> connection, QuicStreamID stream_id, td::Slice data) {
  LOG_EVERY(ERROR) << "unexpected message on " << (connection->is_outbound ? "outbound" : "inbound")
                   << " connection CID:" << connection->cid << " SID:" << stream_id << " size:" << data.size()
                   << " tl_id:" << td::format::as_hex(get_magic(data))
                   << " head:" << td::format::as_hex_dump<4>(data.truncate(32));
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
    // send_message streams register no response: the peer answers with an empty fin (or the stream
    // errors) and completes here with nothing pending. Not an error — the query path registers a
    // response entry, this one does not, so there is nothing to deliver.
    return;
  }

  if (r_data.is_error()) {
    auto error = r_data.move_as_error();
    LOG_EVERY(ERROR) << "Failed to read response stream: " << connection->path << " " << error;
    resp_it->second.set_error(std::move(error));
  } else if (auto answer_R = fetch_tl_object<ton_api::quic_answer>(r_data.ok().clone(), true); answer_R.is_ok()) {
    resp_it->second.set_value(std::move(answer_R.move_as_ok()->data_));
  } else {
    invalid_message(connection, stream_id, r_data.ok());
    resp_it->second.set_error(td::Status::Error("Invalid answer"));
  }
  connection->responses.erase(resp_it);
  // No shutdown needed: the query was sent with fin and a complete answer means the peer fin'd
  // too, so the stream retires on its own (unlike the inbound garbage path above).
}

void QuicSender::on_stream_complete(QuicConnectionId cid, QuicStreamID stream_id, td::Result<td::BufferSlice> r_data) {
  auto it = by_cid_.find(cid);
  if (it == by_cid_.end()) {
    LOG_EVERY(ERROR) << "Unknown CID:" << cid << " SID:" << stream_id;
    return;
  }
  auto connection = it->second;
  if (connection->is_outbound) {
    on_stream_complete_outbound(connection, stream_id, r_data.clone());
  } else {
    on_stream_complete_inbound(connection, stream_id, r_data.clone());
  }
}

void QuicSender::on_stream_closed(QuicConnectionId cid, QuicStreamID stream_id) {
  auto it = by_cid_.find(cid);
  if (it == by_cid_.end()) {
    return;
  }
  auto connection = it->second;
  auto resp_it = connection->responses.find(stream_id);
  if (resp_it == connection->responses.end()) {
    return;
  }
  resp_it->second.set_error(td::Status::Error("stream closed"));
  connection->responses.erase(resp_it);
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
  on_inbound_message(connection, stream_id, std::move(message.data_)).start_immediate().detach();
}

td::actor::Task<> QuicSender::on_inbound_message(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                                                 td::BufferSlice data) {
  // Hold the stream (and its in-flight charge) until the message is actually delivered, not just
  // enqueued — otherwise the fin below would release the charge before adnl handled the message, and
  // a peer could pump messages past the in-flight budget. Then fin the (unidirectional-in-spirit)
  // stream: messages carry no answer.
  auto delivered = co_await td::actor::ask(adnl_, &adnl::AdnlPeerTable::deliver_ex, connection->path.second,
                                           connection->path.first, std::move(data))
                       .wrap();
  LOG_IF(INFO, delivered.is_error()) << "inbound message cid=" << connection->cid << " sid=" << stream_id << ": "
                                     << delivered.error();
  auto sent = co_await td::actor::ask(connection->server, &QuicServer::send_stream, connection->cid,
                                      std::variant<QuicStreamID, StreamOptions>{stream_id}, td::BufferSlice{}, true)
                  .wrap();
  LOG_IF(INFO, sent.is_error()) << "inbound message fin cid=" << connection->cid << " sid=" << stream_id << ": "
                                << sent.error();
  co_return td::Unit{};
}

td::actor::Task<> QuicSender::on_inbound_query(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                                               td::BufferSlice query) {
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
  auto sent = co_await td::actor::ask(connection->server, &QuicServer::send_stream, connection->cid,
                                      std::variant<QuicStreamID, StreamOptions>{stream_id},
                                      create_serialize_tl_object<ton_api::quic_answer>(answer.move_as_ok()), true)
                  .wrap();
  LOG_IF(INFO, sent.is_error()) << "inbound answer cid=" << connection->cid << " sid=" << stream_id << ": "
                                << sent.error();
  co_return td::Unit{};
}

}  // namespace ton::quic
