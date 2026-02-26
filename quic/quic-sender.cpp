#include <utility>

#include "auto/tl/ton_api.hpp"
#include "td/actor/coro_utils.h"
#include "td/utils/Heap.h"
#include "td/utils/as.h"

#include "quic-sender.h"

namespace ton::quic {

static td::uint32 get_magic(const td::BufferSlice &data) {
  return data.size() >= 4 ? td::as<td::uint32>(data.data()) : 0;
}

class QuicSender::ServerCallback final : public QuicServer::Callback {
 public:
  ServerCallback(adnl::AdnlNodeIdShort local_id, td::actor::ActorId<QuicSender> sender)
      : local_id_(local_id), sender_(sender) {
  }

  void on_connected(QuicConnectionId cid, td::SecureString peer_public_key, bool is_outbound) override {
    auto server = td::actor::actor_dynamic_cast<QuicServer>(td::actor::actor_id());
    CHECK(!server.empty());
    td::actor::send_closure(sender_, &QuicSender::on_connected, server, cid, local_id_, std::move(peer_public_key),
                            is_outbound);
  }

  td::Status on_stream(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data, bool is_end) override {
    auto [state_ptr, inserted] = get_or_create_stream(cid, sid);
    if (inserted) {
      td::actor::send_closure(sender_, &QuicSender::init_stream_mtu, cid, sid);
    }
    auto &state = *state_ptr;
    if (state.is_failed()) {
      LOG(INFO) << "got data for closed stream, ignore cid=" << cid << " sid=" << sid;
      return td::Status::Error("stream failed");
    }
    state.append(std::move(data));
    auto status = state.check_limits();
    if (status.is_ok() && !is_end) {
      return td::Status::OK();
    }
    if (status.is_error()) {
      LOG(INFO) << "close stream cid=" << cid << " sid=" << sid << " due to " << status.error();
      fail_stream(state, status.clone());
      return status;
    }
    td::actor::send_closure(sender_, &QuicSender::on_stream_complete, cid, sid, state.extract());
    return td::Status::OK();
  }

  void on_closed(QuicConnectionId cid) override {
    erase_connection(cid);
    td::actor::send_closure(sender_, &QuicSender::on_closed, cid);
  }
  void on_stream_closed(QuicConnectionId cid, QuicStreamID sid) override {
    erase_stream(cid, sid);
  }

  void set_stream_options(QuicConnectionId cid, QuicStreamID sid, StreamOptions options) override {
    auto [state_ptr, inserted] = get_or_create_stream(cid, sid);
    (void)inserted;
    apply_stream_options(*state_ptr, options);
  }

  void loop(td::Timestamp now, StreamShutdownList &shutdown) override {
    while (!timeout_heap_.empty() && td::Timestamp::at(timeout_heap_.top_key()).is_in_past(now)) {
      auto *state = static_cast<StreamState *>(timeout_heap_.pop());
      if (!state->is_failed()) {
        fail_stream(*state, state->timeout_error());
        shutdown.entries.push_back({state->cid, state->sid});
      }
    }
  }

  td::Timestamp next_alarm() const override {
    if (timeout_heap_.empty()) {
      return td::Timestamp::never();
    }
    return td::Timestamp::at(timeout_heap_.top_key());
  }

 private:
  struct StreamState : public td::HeapNode {
    QuicConnectionId cid;
    QuicStreamID sid;

    StreamState(QuicConnectionId cid, QuicStreamID sid) : cid(cid), sid(sid) {
    }

    void append(td::BufferSlice data) {
      CHECK(!failed_);
      if (!data.empty()) {
        builder_.append(std::move(data));
      }
    }

    bool is_failed() const {
      return failed_;
    }

    void mark_failed() {
      failed_ = true;
    }

    td::Status check_limits() const {
      if (failed_) {
        return td::Status::Error("stream already failed");
      }
      auto max_size = options_.max_size.value_or(DEFAULT_STREAM_SIZE_LIMIT);
      if (options_.max_size.has_value() && builder_.size() > max_size) {
        return td::Status::Error(PSLICE() << "stream size limit exceeded: max=" << max_size
                                          << " received=" << builder_.size() << " query_size=" << options_.query_size
                                          << " query_magic=" << td::format::as_hex(options_.query_magic));
      }
      return td::Status::OK();
    }

    td::Status timeout_error() const {
      return td::Status::Error(PSLICE() << "stream timeout exceeded: " << options_.timeout_seconds
                                        << "s query_size=" << options_.query_size << " query_magic="
                                        << td::format::as_hex(options_.query_magic) << " received=" << builder_.size());
    }

    td::BufferSlice extract() {
      return builder_.extract();
    }

    void set_options(StreamOptions options) {
      options_ = options;
    }

   private:
    td::BufferBuilder builder_;
    StreamOptions options_;
    bool failed_{false};
  };

  adnl::AdnlNodeIdShort local_id_;
  td::actor::ActorId<QuicSender> sender_;
  std::map<QuicConnectionId, std::map<QuicStreamID, StreamState>> streams_;
  td::KHeap<double> timeout_heap_;

  std::pair<StreamState *, bool> get_or_create_stream(QuicConnectionId cid, QuicStreamID sid) {
    auto &by_cid = streams_[cid];
    auto [it, inserted] = by_cid.try_emplace(sid, StreamState{cid, sid});
    return {&it->second, inserted};
  }

  void erase_stream(QuicConnectionId cid, QuicStreamID sid) {
    auto cid_it = streams_.find(cid);
    if (cid_it == streams_.end()) {
      return;
    }
    auto &by_sid = cid_it->second;
    auto sid_it = by_sid.find(sid);
    if (sid_it == by_sid.end()) {
      return;
    }
    if (sid_it->second.in_heap()) {
      timeout_heap_.erase(&sid_it->second);
    }
    by_sid.erase(sid_it);
    if (by_sid.empty()) {
      streams_.erase(cid_it);
    }
  }

  void erase_connection(QuicConnectionId cid) {
    auto it = streams_.find(cid);
    if (it == streams_.end()) {
      return;
    }
    for (auto &[sid, state] : it->second) {
      if (state.in_heap()) {
        timeout_heap_.erase(&state);
      }
    }
    streams_.erase(it);
  }

  void apply_stream_options(StreamState &state, const StreamOptions &options) {
    state.set_options(options);
    if (options.timeout) {
      if (state.in_heap()) {
        timeout_heap_.fix(options.timeout.at(), &state);
      } else {
        timeout_heap_.insert(options.timeout.at(), &state);
      }
    } else if (state.in_heap()) {
      timeout_heap_.erase(&state);
    }
  }

  void fail_stream(StreamState &state, td::Status error) {
    if (state.in_heap()) {
      timeout_heap_.erase(&state);
    }
    state.mark_failed();
    td::actor::send_closure(sender_, &QuicSender::on_stream_complete, state.cid, state.sid, std::move(error));
  }
};

static td::Result<adnl::AdnlNodeIdShort> parse_peer_id(td::Slice peer_public_key) {
  if (peer_public_key.size() != 32) {
    return td::Status::Error("peer public key must be 32 bytes");
  }
  td::Bits256 key_bits;
  key_bits.as_slice().copy_from(peer_public_key);
  return adnl::AdnlNodeIdFull(PublicKey(pubkeys::Ed25519(key_bits))).compute_short_id();
}

static td::Result<td::IPAddress> get_ip_address(const adnl::AdnlAddressList &addr_list) {
  td::IPAddress result;
  for (const auto &addr : addr_list.addrs()) {
    auto r_ip = addr->to_ip_address();
    if (r_ip.is_ok() && r_ip.ok().get_port() != 0) {
      result = r_ip.move_as_ok();
    }
  }
  if (!result.is_valid()) {
    return td::Status::Error("no valid ip address");
  }
  return result;
}

static td::Result<std::set<int>> get_unique_ports(const adnl::AdnlAddressList &addr_list) {
  std::set<int> ports;
  for (const auto &addr : addr_list.addrs()) {
    auto r_ip = addr->to_ip_address();
    if (r_ip.is_ok() && r_ip.ok().get_port() != 0) {
      ports.insert(r_ip.ok().get_port());
    }
  }
  if (ports.empty()) {
    return td::Status::Error("no valid ports");
  }
  return ports;
}

QuicSender::QuicSender(td::actor::ActorId<adnl::AdnlPeerTable> adnl, td::actor::ActorId<keyring::Keyring> keyring,
                       QuicServer::Options options)
    : adnl_(std::move(adnl)), keyring_(std::move(keyring)), server_options_(options) {
}

void QuicSender::send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) {
  send_message_coro(src, dst, std::move(data)).start_immediate().detach("quic:send_message");
}

void QuicSender::send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                            td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) {
  connect(std::move(promise), send_query_coro(src, dst, std::move(name), timeout, std::move(data), std::nullopt));
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

void QuicSender::set_udp_offload_options(QuicServer::Options options) {
  server_options_ = options;
}

void QuicSender::add_id(adnl::AdnlNodeIdShort local_id) {
  add_local_id_coro(local_id).start().detach("add local id");
}

void QuicSender::log_stats(std::string reason) {
  for (auto &it : servers_) {
    td::actor::send_closure(it.second.get(), &QuicServer::log_stats, reason);
  }
}

std::vector<metrics::MetricFamily> QuicSender::Stats::Entry::dump() const {
  return {
      metrics::MetricFamily::make_scalar("conns", "gauge", server_stats.total_conns),
      metrics::MetricFamily::make_scalar("rx_bytes_total", "counter", server_stats.impl_stats.bytes_rx),
      metrics::MetricFamily::make_scalar("tx_bytes_total", "counter", server_stats.impl_stats.bytes_tx),
      metrics::MetricFamily::make_scalar("lost_bytes_total", "counter", server_stats.impl_stats.bytes_lost),
      metrics::MetricFamily::make_scalar("unacked_bytes", "gauge", server_stats.impl_stats.bytes_unacked),
      metrics::MetricFamily::make_scalar("unsent_bytes", "gauge", server_stats.impl_stats.bytes_unsent),
      metrics::MetricFamily::make_scalar("open_sids", "gauge", server_stats.impl_stats.open_sids),
      metrics::MetricFamily::make_scalar("mean_rtt", "gauge", server_stats.impl_stats.mean_rtt),
  };
}

std::vector<metrics::MetricFamily> QuicSender::Stats::dump() const {
  auto summary_set = metrics::MetricSet{.families = summary.dump()};
  auto whole_per_path_set = metrics::MetricSet{};
  for (const auto &[path, entry] : per_path) {
    auto path_set = metrics::MetricSet{.families = entry.dump()};
    auto src_v = PSTRING() << path.first, dst_v = PSTRING() << path.second;
    auto label_set = metrics::LabelSet{.labels = {{"src", src_v}, {"dst", dst_v}}};
    whole_per_path_set = std::move(whole_per_path_set).join(std::move(path_set).label(label_set));
  }
  return std::move(summary_set).wrap("summary").join(std::move(whole_per_path_set).wrap("per_path")).families;
}

td::actor::Task<QuicSender::Stats> QuicSender::collect_stats() {
  Stats stats;
  for (auto &[_, server] : servers_) {
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

// TODO(avevad): remove obsolete Stats and collect metrics directly
void QuicSender::collect(td::Promise<metrics::MetricSet> P) {
  td::actor::send_closure(actor_id(this), &QuicSender::collect_stats,
                          td::make_promise([P = std::move(P)](td::Result<Stats> R) mutable {
                            P.set_value(metrics::MetricSet{.families = R.move_as_ok().dump()}.wrap("quic"));
                          }));
}

void QuicSender::on_mtu_updated(td::optional<adnl::AdnlNodeIdShort> local_id,
                                td::optional<adnl::AdnlNodeIdShort> peer_id) {
}

QuicSender::Connection::~Connection() {
  for (auto &[_, P] : responses) {
    P.set_error(td::Status::Error("connection closed"));
  }
}

void QuicSender::start_up() {
  AdnlSenderInterface::start_up();
  alarm_timestamp() = td::Timestamp::now();
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
  auto query_size = data.size();
  auto query_magic = get_magic(data);
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_query>(std::move(data));
  auto cid = conn->cid;
  auto server = conn->server;
  // create stream explicitly to avoid race with response
  auto timeout_seconds = timeout ? timeout.at() - td::Time::now() : 0.0;
  auto stream_limit = get_peer_mtu(src, dst);
  if (limit.has_value())
    stream_limit = std::max(stream_limit, *limit);
  auto stream_id = co_await td::actor::ask(server, &QuicServer::open_stream, cid,
                                           StreamOptions{.max_size = stream_limit,
                                                         .timeout = timeout,
                                                         .timeout_seconds = timeout_seconds,
                                                         .query_size = query_size,
                                                         .query_magic = query_magic});
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

  auto ports = co_await get_unique_ports(node.addr_list());
  if (ports.size() > 1) {
    LOG(WARNING) << "ignoring " << ports.size() - 1 << " redundant ports of local id " << local_id;
  }
  auto port = *ports.begin() + NODE_PORT_OFFSET;

  auto priv_key = co_await td::actor::ask(keyring_, &keyring::Keyring::export_private_key, local_id.pubkey_hash());
  auto ed25519_key = co_await priv_key.export_as_ed25519();
  local_keys_.emplace(local_id, td::Ed25519::PrivateKey(ed25519_key.as_octet_string()));

  if (servers_.find(local_id) != servers_.end()) {
    LOG(INFO) << "Local id has already been added: " << local_id;
    co_return td::Unit{};  // already added
  }

  auto server = co_await QuicServer::create(port, td::Ed25519::PrivateKey(local_keys_.at(local_id).as_octet_string()),
                                            std::make_unique<ServerCallback>(local_id, actor_id(this)), "ton",
                                            "0.0.0.0", server_options_);
  servers_[local_id] = std::move(server);

  co_return td::Unit{};
}

td::actor::Task<std::shared_ptr<QuicSender::Connection>> QuicSender::find_or_create_connection(AdnlPath path) {
  std::shared_ptr<Connection> connection;
  auto iter = outbound_.find(path);
  if (iter == outbound_.end()) {
    connection = std::make_shared<Connection>();
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
  auto R = co_await init_connection_inner(path, connection).wrap();
  if (R.is_ok()) {
    co_return td::Unit{};  // wait for on_ready
  }

  LOG(WARNING) << "Failed to init connection: " << path << " " << R.error();
  // got error before connection created
  auto promises = std::move(connection->waiting_ready);
  for (auto &promise : promises) {
    promise.set_result(R.error().clone());
  }
  CHECK(outbound_.erase(path) == 1);
  co_return td::Unit{};
}

td::actor::Task<td::Unit> QuicSender::init_connection_inner(AdnlPath path, std::shared_ptr<Connection> conn) {
  auto node = co_await ask(adnl_, &adnl::Adnl::get_peer_node, path.first, path.second).trace("get_peer_node");

  auto peer_addr = co_await get_ip_address(node.addr_list());
  auto peer_host = peer_addr.get_ip_host();
  auto peer_port = peer_addr.get_port() + NODE_PORT_OFFSET;

  auto local_key_iter = local_keys_.find(path.first);
  if (local_key_iter == local_keys_.end()) {
    co_return td::Status::Error("no local key for source ADNL ID");
  }

  auto client_key = td::Ed25519::PrivateKey(local_key_iter->second.as_octet_string());

  auto server_iter = servers_.find(path.first);
  if (server_iter == servers_.end()) {
    co_return td::Status::Error("no QuicServer for local id");
  }

  auto server = server_iter->second.get();
  auto connection_id =
      co_await ask(server, &QuicServer::connect, peer_host, peer_port, std::move(client_key), td::Slice("ton"))
          .trace("connect");
  conn->cid = connection_id;
  conn->path = path;
  conn->server = server;
  CHECK(by_cid_.emplace(connection_id, conn).second);
  co_return td::Unit{};
}

void QuicSender::init_stream_mtu(QuicConnectionId cid, QuicStreamID sid) {
  auto [src, dst] = by_cid_.at(cid)->path;
  auto mtu = get_peer_mtu(src, dst);
  auto server = servers_.at(src).get();
  td::actor::send_closure(server, &QuicServer::change_stream_options, cid, sid, StreamOptions{mtu});
}

void QuicSender::on_connected(td::actor::ActorId<QuicServer> server, QuicConnectionId cid,
                              adnl::AdnlNodeIdShort local_id, td::SecureString peer_public_key, bool is_outbound) {
  auto r_peer_id = parse_peer_id(peer_public_key.as_slice());
  if (r_peer_id.is_error()) {
    LOG(ERROR) << "Failed to parse public key " << r_peer_id.error();
    return;
  }
  auto peer_id = r_peer_id.move_as_ok();

  if (get_peer_mtu(local_id, peer_id) == 0) {
    LOG(WARNING) << "Dropping connection for MTU 0 path [" << local_id << ';' << peer_id << ']';
    return;
  }

  auto path = AdnlPath{local_id, peer_id};
  std::shared_ptr<Connection> connection;
  td::Result<td::Unit> result;
  if (auto it = by_cid_.find(cid); it != by_cid_.end()) {
    connection = it->second;
    if (connection->path != path) {
      result = td::Status::Error(PSLICE() << "Key mismatch got:" << path << " expected " << connection->path);
    } else {
      result = td::Unit{};
    }
  } else {
    if (is_outbound) {
      LOG(ERROR) << "Unknown outbound connection";
    }
    // Close existing inbound connection for same path if any
    LOG(ERROR) << "Create inbound " << path;
    if (auto old_it = inbound_.find(path); old_it != inbound_.end()) {
      auto old_conn = old_it->second;
      by_cid_.erase(old_conn->cid);
      td::actor::send_closure(old_conn->server, &QuicServer::close, old_conn->cid);
      inbound_.erase(old_it);
    }
    connection = std::make_shared<Connection>();
    connection->server = server;
    connection->path = path;
    connection->cid = cid;
    connection->is_ready = true;
    CHECK(by_cid_.emplace(cid, connection).second);
    inbound_[path] = connection;
    result = td::Unit{};
  }

  connection->is_ready = true;
  auto promises = std::move(connection->waiting_ready);
  for (auto &promise : promises) {
    promise.set_result(result.clone());
  }
}

void QuicSender::on_stream_complete(QuicConnectionId cid, QuicStreamID stream_id, td::Result<td::BufferSlice> r_data) {
  auto it = by_cid_.find(cid);
  if (it == by_cid_.end()) {
    LOG(ERROR) << "Unknown CID:" << cid << " SID:" << stream_id;
    return;
  }
  auto connection = it->second;

  if (r_data.is_error()) {
    auto resp_it = connection->responses.find(stream_id);
    if (resp_it != connection->responses.end()) {
      resp_it->second.set_error(r_data.move_as_error());
      connection->responses.erase(resp_it);
    }
    return;
  }

  auto data = r_data.move_as_ok();
  if (data.empty()) {
    return;  // currently message will trigger empty response
  }

  // TODO: accept request only from inbound streams. and answers only from outbound

  auto req_R = fetch_tl_object<ton_api::quic_Request>(data.clone(), true);
  if (req_R.is_ok()) {
    auto request = req_R.move_as_ok();
    ton_api::downcast_call(*request, [&](auto &query) { on_request(connection, stream_id, query); });
    return;
  }

  auto answer_R = fetch_tl_object<ton_api::quic_answer>(data.clone(), true);
  if (answer_R.is_ok()) {
    on_answer(*connection, stream_id, *answer_R.move_as_ok());
    return;
  }

  LOG(ERROR) << "malformed message from CID:" << cid << " SID:" << stream_id << " size:" << data.size()
             << " tl_id:" << td::format::as_hex(get_magic(data))
             << " head:" << td::format::as_hex_dump<4>(data.as_slice().truncate(32));
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
    auto c = outbound_.extract(out_it).mapped();
    for (auto &p : c->waiting_ready) {
      p.set_result(td::Status::Error("connection closed"));
    }
  }
  if (auto in_it = inbound_.find(path); in_it != inbound_.end() && in_it->second->cid == cid) {
    auto c = inbound_.extract(in_it).mapped();
    for (auto &p : c->waiting_ready) {
      p.set_result(td::Status::Error("connection closed"));
    }
  }
}

void QuicSender::on_request(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                            ton_api::quic_query &query) {
  on_inbound_query(connection, stream_id, std::move(query.data_)).start_immediate().detach();
}

void QuicSender::on_request(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                            ton_api::quic_message &message) {
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver, connection->path.second, connection->path.first,
                          std::move(message.data_));
  // TODO: use unidirectional stream, so there will be no need to process result
  td::actor::send_closure(connection->server, &QuicServer::send_stream, connection->cid, stream_id, td::BufferSlice{},
                          true);
}

td::actor::Task<> QuicSender::on_inbound_query(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                                               td::BufferSlice query) {
  auto answer = co_await td::actor::ask(adnl_, &adnl::AdnlPeerTable::deliver_query, connection->path.second,
                                        connection->path.first, std::move(query));
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_answer>(std::move(answer));
  td::actor::send_closure(connection->server, &QuicServer::send_stream, connection->cid, stream_id,
                          std::move(wire_data), true);
  co_return td::Unit{};
}

void QuicSender::on_answer(Connection &connection, QuicStreamID stream_id, ton_api::quic_answer &answer) {
  auto it = connection.responses.find(stream_id);
  if (it == connection.responses.end()) {
    LOG(ERROR) << "Answer from unknown stream_id";
    return;
  }
  it->second.set_result(std::move(answer.data_));
  connection.responses.erase(it);
}

}  // namespace ton::quic
