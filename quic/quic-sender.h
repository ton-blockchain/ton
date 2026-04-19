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

  void set_quic_options(QuicServer::Options options);
  void add_id(adnl::AdnlNodeIdShort local_id) override;
  void add_protected_peers(adnl::AdnlNodeIdShort local_id, std::vector<adnl::AdnlNodeIdShort> peer_ids) override;
  void remove_protected_peers(adnl::AdnlNodeIdShort local_id, std::vector<adnl::AdnlNodeIdShort> peer_ids) override;
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
    bool is_outbound = false;
    QuicConnectionId cid{};
    AdnlPath path{};
    td::actor::ActorId<QuicServer> server;
    std::vector<td::Promise<td::Unit>> waiting_ready{};
    std::optional<td::Status> init_error{};
    std::unordered_map<QuicStreamID, td::Promise<td::BufferSlice>> responses{};

    ~Connection();
  };

  class ServerCallback;

  static constexpr int NODE_PORT_OFFSET = 1000;

  td::actor::ActorId<adnl::AdnlPeerTable> adnl_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  QuicServer::Options server_options_;

  std::map<AdnlPath, std::shared_ptr<Connection>> outbound_;
  std::map<AdnlPath, std::shared_ptr<Connection>> inbound_;
  std::map<QuicConnectionId, std::shared_ptr<Connection>> by_cid_;
  struct ProtectedPeerState {
    size_t refs = 0;
    std::optional<td::IPAddress> endpoint;
    bool resolve_in_flight = false;
    size_t resolve_failures = 0;
    td::uint64 resolve_generation = 0;
  };
  std::map<adnl::AdnlNodeIdShort, std::map<adnl::AdnlNodeIdShort, ProtectedPeerState>> protected_peers_;
  std::map<adnl::AdnlNodeIdShort, std::map<td::IPAddress, size_t>> protected_endpoint_refs_;
  td::Timestamp next_protected_endpoints_log_at_ = td::Timestamp::never();
  td::uint64 next_protected_peer_resolve_generation_ = 1;

  std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<QuicServer>> servers_;
  std::map<adnl::AdnlNodeIdShort, td::Ed25519::PrivateKey> local_keys_;

  void start_up() override;
  void alarm() override;

  static constexpr double PROTECTED_ENDPOINTS_LOG_PERIOD = 300.0;
  static constexpr double PROTECTED_ENDPOINTS_INITIAL_LOG_DELAY = 30.0;
  static constexpr double PROTECTED_PEER_RESOLVE_RETRY_MAX_DELAY = 60.0;
  static constexpr size_t PROTECTED_ENDPOINTS_LOG_LIMIT = 32;
  static constexpr size_t PROTECTED_UNRESOLVED_LOG_LIMIT = 16;

  td::actor::Task<td::Unit> send_message_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                              td::BufferSlice data);
  td::actor::Task<td::Unit> send_message_coro_inner(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                    td::BufferSlice data);
  td::actor::Task<td::BufferSlice> send_query_coro(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst,
                                                   std::string name, td::Timestamp timeout, td::BufferSlice data,
                                                   std::optional<td::uint64> limit);
  td::actor::Task<std::string> get_conn_ip_str_coro(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id);
  td::actor::Task<> add_local_id_coro(adnl::AdnlNodeIdShort local_id);
  td::actor::Task<> resolve_protected_peer_endpoint(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                                    td::uint64 generation);

  td::actor::Task<std::shared_ptr<Connection>> find_or_create_connection(AdnlPath path);
  td::actor::Task<td::Unit> init_connection(AdnlPath path, std::shared_ptr<Connection> connection);
  td::actor::Task<td::Unit> init_connection_inner(AdnlPath path, std::shared_ptr<Connection> conn);
  void finish_connection_init(const std::shared_ptr<Connection>& connection, td::Result<td::Unit> result);

  td::Result<td::Unit> on_connected_inner(td::actor::ActorId<QuicServer> server, QuicConnectionId cid,
                                          adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                          bool is_outbound, std::shared_ptr<Connection>& connection);

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
  void update_protected_peer_endpoint(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                      std::optional<td::IPAddress> endpoint);
  void schedule_protected_peer_endpoint_resolve(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id);
  void finish_protected_peer_endpoint_resolve(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                              td::uint64 generation);
  bool is_protected_peer_registered(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                    td::uint64 generation) const;
  void schedule_protected_endpoints_log(double delay = PROTECTED_ENDPOINTS_INITIAL_LOG_DELAY);
  void maybe_log_protected_endpoints(std::string reason, bool force = false);
  void add_protected_endpoint_ref(adnl::AdnlNodeIdShort local_id, const td::IPAddress& endpoint, size_t refs = 1);
  void remove_protected_endpoint_ref(adnl::AdnlNodeIdShort local_id, const td::IPAddress& endpoint, size_t refs = 1);

  static td::Result<td::IPAddress> get_ip_address(const adnl::AdnlNode& node);
};

}  // namespace ton::quic
