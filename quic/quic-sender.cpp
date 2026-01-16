#include <utility>

#include "adnl/adnl-peer-table.h"
#include "auto/tl/ton_api.hpp"
#include "td/actor/coro_utils.h"

#include "quic-sender.h"

namespace ton::quic {

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

  void on_stream(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data, bool is_end) override {
    auto [cid_it, cid_inserted] = streams_.emplace(cid, std::map<QuicStreamID, StreamState>{});
    auto [sid_it, sid_inserted] = cid_it->second.emplace(sid, StreamState{});
    (void)cid_inserted;
    (void)sid_inserted;
    auto &state = sid_it->second;
    state.append(std::move(data));
    if (state.should_end()) {
      is_end = true;
    }
    if (!is_end) {
      return;
    }
    auto complete_data = state.extract();
    cid_it->second.erase(sid_it);
    if (cid_it->second.empty()) {
      streams_.erase(cid_it);
    }
    td::actor::send_closure(sender_, &QuicSender::on_stream_complete, cid, sid, std::move(complete_data));
  }

  void on_closed(QuicConnectionId cid) override {
    streams_.erase(cid);
    td::actor::send_closure(sender_, &QuicSender::on_closed, cid);
  }

  void set_stream_options(QuicConnectionId cid, QuicStreamID sid, StreamOptions options) override {
    streams_[cid][sid].set_options(options);
  }

 private:
  struct StreamState {
    void append(td::BufferSlice data) {
      if (!data.empty()) {
        builder_.append(std::move(data));
      }
    }

    bool should_end() const {
      if (options_.max_size.has_value() && builder_.size() > *options_.max_size) {
        return true;
      }
      if (options_.timeout && options_.timeout.is_in_past()) {
        return true;
      }
      return false;
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
  };

  adnl::AdnlNodeIdShort local_id_;
  td::actor::ActorId<QuicSender> sender_;
  std::map<QuicConnectionId, std::map<QuicStreamID, StreamState>> streams_;
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

QuicSender::QuicSender(td::actor::ActorId<adnl::AdnlPeerTable> adnl, td::actor::ActorId<keyring::Keyring> keyring)
    : adnl_(std::move(adnl)), keyring_(std::move(keyring)) {
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

void QuicSender::add_local_id(adnl::AdnlNodeIdShort local_id) {
  add_local_id_coro(local_id).start().detach("add local id");
}

QuicSender::Connection::~Connection() {
  for (auto &[_, P] : responses) {
    P.set_error(td::Status::Error("connection closed"));
  }
}

td::actor::Task<td::Unit> QuicSender::send_message_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                        td::BufferSlice data) {
  auto conn = co_await find_or_create_connection({src, dst});
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_message>(std::move(data));
  co_await td::actor::ask(conn->server, &QuicServer::send_stream, conn->cid, StreamOptions{}, std::move(wire_data),
                          true);
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
                                           StreamOptions{.max_size = limit, .timeout = timeout});
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
    co_return td::Status::Error(PSLICE() << "Local id has already been added: " << local_id);
  }

  auto server = co_await QuicServer::create(port, td::Ed25519::PrivateKey(local_keys_.at(local_id).as_octet_string()),
                                            std::make_unique<ServerCallback>(local_id, actor_id(this)));
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

  // got error befor connection created
  auto promises = std::move(connection->waiting_ready);
  for (auto &promise : promises) {
    promise.set_result(R.error().clone());
  }
  CHECK(outbound_.erase(path) == 1);
  co_return td::Unit{};
}

td::actor::Task<td::Unit> QuicSender::init_connection_inner(AdnlPath path, std::shared_ptr<Connection> conn) {
  auto node = co_await ask(adnl_, &adnl::Adnl::get_peer_node, path.first, path.second);

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
      co_await ask(server, &QuicServer::connect, peer_host, peer_port, std::move(client_key), td::Slice("ton"));
  conn->cid = connection_id;
  conn->path = path;
  conn->server = server;
  CHECK(by_cid_.emplace(connection_id, conn).second);
  co_return td::Unit{};
}

void QuicSender::on_connected(td::actor::ActorId<QuicServer> server, QuicConnectionId cid,
                              adnl::AdnlNodeIdShort local_id, td::SecureString peer_public_key, bool is_outbound) {
  auto r_peer_id = parse_peer_id(peer_public_key.as_slice());
  if (r_peer_id.is_error()) {
    LOG(ERROR) << "Failed to parse public key " << r_peer_id.error();
    return;
  }
  auto peer_id = r_peer_id.move_as_ok();

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

void QuicSender::on_stream_complete(QuicConnectionId cid, QuicStreamID stream_id, td::BufferSlice data) {
  if (data.empty()) {
    return;  // currently message will trigger empty response
  }

  auto it = by_cid_.find(cid);
  if (it == by_cid_.end()) {
    return;
  }
  auto connection = it->second;

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

  LOG(ERROR) << "malformed message from CID, SID:" << stream_id;
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
