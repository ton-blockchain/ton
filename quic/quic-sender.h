#pragma once

#include "adnl/adnl-peer-table.h"
#include "adnl/adnl.h"
#include "keyring/keyring.h"
#include "td/actor/coro_task.h"

#include "openssl-utils.h"
#include "quic-server.h"

namespace ton::quic {

class QuicSender : public adnl::AdnlSenderInterface {
 public:
  using AdnlPath = std::pair<adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort>;

  explicit QuicSender(td::actor::ActorId<adnl::AdnlPeerTable> adnl, td::actor::ActorId<keyring::Keyring> keyring);
  ~QuicSender() override = default;

  void send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override;
  void send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override;
  void send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                     td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                     td::uint64 max_answer_size) override;
  void get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                       td::Promise<td::string> promise) override;

  void add_local_id(adnl::AdnlNodeIdShort local_id);

 private:
  struct Connection {
    bool init_started = false;
    bool is_ready = false;
    QuicConnectionId cid{};
    AdnlPath path{};
    td::actor::ActorId<QuicServer> server;
    std::vector<td::Promise<td::Unit>> waiting_ready{};
    std::unordered_map<QuicStreamID, td::Promise<td::BufferSlice>> responses{};
  };

  class ServerCallback;

  static constexpr int NODE_PORT_OFFSET = 1000;

  td::actor::ActorId<adnl::AdnlPeerTable> adnl_;
  td::actor::ActorId<keyring::Keyring> keyring_;

  std::map<AdnlPath, std::shared_ptr<Connection>> outbound_;
  std::map<AdnlPath, std::shared_ptr<Connection>> inbound_;
  std::map<QuicConnectionId, std::shared_ptr<Connection>> by_cid_;

  std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<QuicServer>> servers_;
  std::map<adnl::AdnlNodeIdShort, td::Ed25519::PrivateKey> local_keys_;

  td::actor::Task<td::Unit> send_message_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                              td::BufferSlice data);
  td::actor::Task<td::BufferSlice> send_query_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                   std::string name, td::Timestamp timeout, td::BufferSlice data);
  td::actor::Task<std::string> get_conn_ip_str_coro(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id);
  td::actor::Task<> add_local_id_coro(adnl::AdnlNodeIdShort local_id);

  td::actor::Task<std::shared_ptr<Connection>> find_or_create_connection(AdnlPath path);
  td::actor::Task<td::Unit> init_connection(AdnlPath path, std::shared_ptr<Connection> connection);
  td::actor::Task<td::Unit> init_connection_inner(AdnlPath path, std::shared_ptr<Connection> conn);

  td::Status on_connected(td::actor::ActorId<QuicServer> server, QuicConnectionId cid, adnl::AdnlNodeIdShort local_id,
                          td::SecureString peer_public_key, bool is_outbound);
  void on_stream(QuicConnectionId cid, QuicStreamID stream_id, td::BufferSlice data);
  void on_closed(QuicConnectionId cid);

  void on_request(std::shared_ptr<Connection> connection, QuicStreamID stream_id, ton_api::quic_query& query);
  void on_request(std::shared_ptr<Connection> connection, QuicStreamID stream_id, ton_api::quic_message& message);
  td::actor::Task<> on_inbound_query(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                                     td::BufferSlice query);
  void on_answer(Connection& connection, QuicStreamID stream_id, ton_api::quic_answer& answer);
};

}  // namespace ton::quic
