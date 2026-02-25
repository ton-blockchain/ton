#pragma once

#include <string>

#include "adnl/adnl-peer-table.h"
#include "adnl/adnl-sender-ex.h"
#include "keyring/keyring.h"
#include "metrics/metrics-collectors.h"
#include "td/actor/coro_task.h"

#include "quic-server.h"

namespace ton::quic {

class QuicSender : public adnl::AdnlSenderEx, public virtual metrics::AsyncCollector {
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

  void set_udp_offload_options(QuicServer::Options options);
  void add_id(adnl::AdnlNodeIdShort local_id) override;
  void log_stats(std::string reason = "stats");

  struct Stats {
    struct Entry {
      QuicServer::Stats::Entry server_stats = {};

      Entry operator+(const Entry& other) const {
        return {.server_stats = server_stats + other.server_stats};
      }

      [[nodiscard]] std::vector<metrics::MetricFamily> dump() const;
    } summary = {};
    std::map<AdnlPath, Entry> per_path;

    [[nodiscard]] std::vector<metrics::MetricFamily> dump() const;
  };

  td::actor::Task<Stats> collect_stats();
  void collect(td::Promise<metrics::MetricSet> P) override;

 protected:
  void on_mtu_updated(td::optional<adnl::AdnlNodeIdShort> local_id,
                      td::optional<adnl::AdnlNodeIdShort> peer_id) override;

 private:
  struct Connection {
    bool init_started = false;
    bool is_ready = false;
    QuicConnectionId cid{};
    AdnlPath path{};
    td::actor::ActorId<QuicServer> server;
    std::vector<td::Promise<td::Unit>> waiting_ready{};
    std::unordered_map<QuicStreamID, td::Promise<td::BufferSlice>> responses{};

    ~Connection();
  };

  class ServerCallback;

  static constexpr int NODE_PORT_OFFSET = 1000;

  static constexpr size_t DEFAULT_STREAM_SIZE_LIMIT = 1 * 1024 * 1024;  // 1 MiB

  td::actor::ActorId<adnl::AdnlPeerTable> adnl_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  QuicServer::Options server_options_;

  std::map<AdnlPath, std::shared_ptr<Connection>> outbound_;
  std::map<AdnlPath, std::shared_ptr<Connection>> inbound_;
  std::map<QuicConnectionId, std::shared_ptr<Connection>> by_cid_;

  std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<QuicServer>> servers_;
  std::map<adnl::AdnlNodeIdShort, td::Ed25519::PrivateKey> local_keys_;

  void start_up() override;

  td::actor::Task<td::Unit> send_message_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                              td::BufferSlice data);
  td::actor::Task<td::Unit> send_message_coro_inner(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                    td::BufferSlice data);
  td::actor::Task<td::BufferSlice> send_query_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                   std::string name, td::Timestamp timeout, td::BufferSlice data,
                                                   std::optional<td::uint64> limit);
  td::actor::Task<std::string> get_conn_ip_str_coro(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id);
  td::actor::Task<> add_local_id_coro(adnl::AdnlNodeIdShort local_id);

  td::actor::Task<std::shared_ptr<Connection>> find_or_create_connection(AdnlPath path);
  td::actor::Task<td::Unit> init_connection(AdnlPath path, std::shared_ptr<Connection> connection);
  td::actor::Task<td::Unit> init_connection_inner(AdnlPath path, std::shared_ptr<Connection> conn);

  void init_stream_mtu(QuicConnectionId cid, QuicStreamID sid);

  void on_connected(td::actor::ActorId<QuicServer> server, QuicConnectionId cid, adnl::AdnlNodeIdShort local_id,
                    td::SecureString peer_public_key, bool is_outbound);
  void on_stream_complete(QuicConnectionId cid, QuicStreamID stream_id, td::Result<td::BufferSlice> data);
  void on_closed(QuicConnectionId cid);

  void on_request(std::shared_ptr<Connection> connection, QuicStreamID stream_id, ton_api::quic_query& query);
  void on_request(std::shared_ptr<Connection> connection, QuicStreamID stream_id, ton_api::quic_message& message);
  td::actor::Task<> on_inbound_query(std::shared_ptr<Connection> connection, QuicStreamID stream_id,
                                     td::BufferSlice query);
  void on_answer(Connection& connection, QuicStreamID stream_id, ton_api::quic_answer& answer);
};

}  // namespace ton::quic
