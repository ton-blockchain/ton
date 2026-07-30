#pragma once

#include <string>

#include "adnl/adnl-peer-table.h"
#include "adnl/adnl-sender-ex.h"
#include "keyring/keyring.h"
#include "metrics/collectors.h"
#include "metrics/well-known.h"
#include "td/actor/coro_task.h"
#include "td/utils/Timer.h"

#include "quic-server.h"

namespace ton::quic {

class QuicSender : public adnl::AdnlSenderEx {
 public:
  using AdnlPath = std::pair<adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort>;

  explicit QuicSender(td::actor::ActorId<adnl::AdnlPeerTable> adnl, td::actor::ActorId<keyring::Keyring> keyring,
                      QuicServer::Options options = {});
  ~QuicSender() override = default;

  void send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override;
  void send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override;
  void send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                     td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                     td::uint64 max_answer_size) override;
  void get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                       td::Promise<td::string> promise) override;

  void set_quic_options(QuicServer::Options options);
  void add_id(adnl::AdnlNodeIdShort local_id) override;
  void log_stats(std::string reason = "stats");

  td::actor::Task<> collect(metrics::Context ctx);

 protected:
  void on_mtu_updated(td::optional<adnl::AdnlNodeIdShort> local_id,
                      td::optional<adnl::AdnlNodeIdShort> peer_id) override;

 private:
  struct Connection {
    // An outbound message awaiting the empty response the peer answers it with.
    struct PendingMessage {
      td::int32 magic;
      td::Timer timer;
    };

    bool init_started = false;
    bool is_ready = false;
    bool is_outbound = false;
    QuicConnectionId cid{};
    AdnlPath path{};
    td::actor::ActorId<QuicServer> server;
    std::vector<td::Promise<td::Unit>> waiting_ready{};
    std::optional<td::Status> init_error{};
    std::unordered_map<QuicStreamID, td::Promise<td::BufferSlice>> responses{};
    std::unordered_map<QuicStreamID, PendingMessage> messages{};

    ~Connection();
  };

  class ServerCallback;

  static constexpr int NODE_PORT_OFFSET = 1000;

  td::actor::ActorId<adnl::AdnlPeerTable> adnl_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  QuicServer::Options server_options_;

  metrics::App app_;
  metrics::TlLatencyBucket query_roundtrip_{"quic query roundtrip", "seconds"};
  metrics::TlLatencyBucket message_delivery_{"quic message delivery", "seconds"};

  std::map<AdnlPath, std::shared_ptr<Connection>> outbound_;
  std::map<AdnlPath, std::shared_ptr<Connection>> inbound_;
  std::map<QuicConnectionId, std::shared_ptr<Connection>> by_cid_;

  std::map<int, td::actor::ActorOwn<QuicServer>> servers_by_port_;
  std::map<int, ServerStats> last_server_stats_;
  std::map<adnl::AdnlNodeIdShort, td::actor::ActorId<QuicServer>> servers_by_id_;
  std::map<adnl::AdnlNodeIdShort, td::Ed25519::PrivateKey> local_keys_;

  void start_up() override;

  td::actor::Task<td::Unit> send_message_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                              td::BufferSlice data);
  td::actor::Task<td::Unit> send_message_coro_inner(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                    td::BufferSlice data, td::int32 magic);
  td::actor::Task<td::BufferSlice> send_query_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                   std::string name, td::Timestamp timeout, td::BufferSlice data,
                                                   std::optional<td::uint64> limit);
  // Takes a ready connection: the round-trip timer is already running by the time it is entered.
  td::actor::Task<td::BufferSlice> send_query_coro_inner(std::shared_ptr<Connection> conn, StreamOptions options,
                                                         td::BufferSlice data);
  td::actor::Task<std::string> get_conn_ip_str_coro(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id);
  td::actor::Task<> add_local_id_coro(adnl::AdnlNodeIdShort local_id);

  td::actor::Task<std::shared_ptr<Connection>> find_or_create_connection(AdnlPath path);
  td::actor::Task<td::Unit> init_connection(AdnlPath path, std::shared_ptr<Connection> connection);
  td::actor::Task<td::Unit> init_connection_inner(AdnlPath path, std::shared_ptr<Connection> conn);
  void finish_connection_init(const std::shared_ptr<Connection>& connection, td::Result<td::Unit> result);

  // `reject_reason` classifies the failure for QuicServer's transport_dropped counter; it is
  // meaningless when the call succeeds.
  td::Result<td::Unit> on_connected_inner(td::actor::ActorId<QuicServer> server, QuicConnectionId cid,
                                          adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                          bool is_outbound, std::shared_ptr<Connection>& connection,
                                          metrics::Reason& reject_reason);

  void on_connected(td::actor::ActorId<QuicServer> server, QuicConnectionId cid, adnl::AdnlNodeIdShort local_id,
                    adnl::AdnlNodeIdShort peer_id, bool is_outbound);
  void on_stream_complete(QuicConnectionId cid, QuicStreamID stream_id, td::Result<td::BufferSlice> data);
  void on_stream_closed(QuicConnectionId cid, QuicStreamID stream_id);
  void on_closed(QuicConnectionId cid);

  void on_request(std::shared_ptr<Connection> connection, QuicStreamID stream_id, ton_api::quic_query& query);
  void on_request(std::shared_ptr<Connection> connection, QuicStreamID stream_id, ton_api::quic_message& message);
  td::actor::Task<> on_inbound_query(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                                     td::BufferSlice query);
  void on_answer(Connection& connection, QuicStreamID stream_id, ton_api::quic_answer& answer);
  // Closes an outbound message's delivery entry, if the stream carried one.
  void record_message_delivery(Connection& connection, QuicStreamID stream_id, bool ok);

  static td::Result<td::IPAddress> get_ip_address(const adnl::AdnlNode& node);
};

}  // namespace ton::quic
