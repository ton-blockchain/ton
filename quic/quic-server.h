#pragma once

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <variant>

#include "adnl/adnl-node-id.hpp"
#include "adnl/utils.hpp"
#include "crypto/common/refcnt.hpp"
#include "metrics/well-known.h"
#include "td/actor/ActorOwn.h"
#include "td/actor/core/Actor.h"
#include "td/utils/Heap.h"
#include "td/utils/buffer.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/UdpSocketFd.h"

#include "Ed25519.h"
#include "metrics.h"
#include "quic-common.h"
#include "quic-connection-rate-limiters.h"

namespace ton::quic {
struct QuicConnectionOptions;
struct ServerIdentities;
struct ServerInitialInfo;
struct VersionCid;

struct QuicConnectionPImpl;

struct StreamOptions {
  std::optional<td::uint64> max_size;
  td::Timestamp timeout = td::Timestamp::never();
  double timeout_seconds = 0.0;
  td::uint64 query_size = 0;
  td::int32 query_magic = 0;  // 0 on inbound streams: no query of ours to describe
};

struct StreamShutdownList {
  struct Entry {
    QuicConnectionId cid;
    QuicStreamID sid;
  };
  td::vector<Entry> entries;
};

class QuicServer : public td::actor::Actor, public td::ObserverBase {
 public:
  struct Options {
    bool enable_gso = true;
    bool enable_gro = true;
    bool enable_mmsg = true;
    CongestionControlAlgo cc_algo = CongestionControlAlgo::Bbr;
    std::optional<size_t> flood_control = DEFAULT_FLOOD_CONTROL;
    std::optional<size_t> max_streams_bidi = std::nullopt;
    std::optional<size_t> ack_thresh = std::nullopt;
    std::optional<double> max_ack_delay_seconds = std::nullopt;
    td::uint32 new_connection_rate_limit_capacity = 10;
    double new_connection_rate_limit_period = 0.2;
    td::uint32 global_new_connection_rate_limit_capacity = 100000;
    double global_new_connection_rate_limit_period = 0.00001;
    bool stateless_retry = true;
  };
  class Callback {
   public:
    virtual td::Status on_connected(QuicConnectionId cid, td::SecureString local_public_key,
                                    td::SecureString peer_public_key, bool is_outbound) = 0;
    virtual td::Status on_stream(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data, bool is_end) = 0;
    virtual void on_closed(QuicConnectionId cid) = 0;
    virtual void on_stream_closed(QuicConnectionId cid, QuicStreamID sid) = 0;
    virtual void set_stream_options(QuicConnectionId cid, QuicStreamID sid, StreamOptions options) {
    }
    virtual void loop(td::Timestamp now, StreamShutdownList &streams_to_shutdown) {
    }
    virtual td::Timestamp next_alarm() const {
      return td::Timestamp::never();
    }
    virtual void set_peer_mtu_callback(std::function<td::uint64(adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort)> f) = 0;
    virtual ~Callback() = default;
  };

  void send_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data);
  void send_stream_end(QuicConnectionId cid, QuicStreamID sid);
  td::Result<QuicStreamID> open_stream(QuicConnectionId cid, StreamOptions options = {});

  td::Result<QuicStreamID> send_stream(QuicConnectionId cid, std::variant<QuicStreamID, StreamOptions> stream,
                                       td::BufferSlice data, bool is_end);

  td::Result<QuicConnectionId> connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key, td::Slice alpn,
                                       td::Slice sni);

  void shutdown_stream(QuicConnectionId cid, QuicStreamID sid);
  void on_connection_closed(QuicConnectionId cid);
  void log_stats(std::string reason = "stats");

  // A handshake's direction is who dialled: `out` is us, `in` is the peer.
  static metrics::Direction direction_of(bool is_outbound) {
    return is_outbound ? metrics::Direction::out : metrics::Direction::in;
  }

  // Counts a handshake the application rejected after Callback::on_connected already returned OK,
  // i.e. from another actor, under the reason that actor decided on. Callers that reject
  // synchronously are counted at the callback site, which calls this too.
  void record_handshake_reject(metrics::Reason reason, bool is_outbound) {
    record_transport_dropped(metrics::Direction::in, reason);
    transport_stats_.handshakes.at(direction_of(is_outbound), HandshakeResult::rejected).inc();
  }

  // Counts a handshake the application accepted: the connection is ready to carry traffic. Disjoint
  // from record_handshake_reject() — an accepting application never reports a rejection, and a
  // rejected connection is torn down instead of becoming ready.
  void record_handshake_completed(bool is_outbound) {
    transport_stats_.handshakes.at(direction_of(is_outbound), HandshakeResult::completed).inc();
  }

  // MTU state is keyed by local_id and (local_id, peer_id). Peer-specific MTU overrides the
  // per-local default. Setting an MTU to 0 erases the corresponding entry.
  void set_default_mtu(adnl::AdnlNodeIdShort local_id, td::uint64 mtu);
  void set_peer_mtu(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id, td::uint64 mtu);

  // Register an additional ADNL identity on this server. Subsequent identities are reachable via
  // SNI (ServerIdentity::sni(local_id)). Re-registering an id that's already present is a no-op.
  void add_identity(adnl::AdnlNodeIdShort local_id, td::Ed25519::PrivateKey key);

  constexpr static size_t DEFAULT_FLOOD_CONTROL = 1000;

  QuicServer(td::UdpSocketFd fd, td::uint64 default_mtu, ServerIdentity identity, td::BufferSlice alpn,
             std::unique_ptr<Callback> callback, Options options);

  static td::Result<td::actor::ActorOwn<QuicServer>> create(int port, std::unique_ptr<Callback> callback,
                                                            td::uint64 default_mtu, ServerIdentity identity,
                                                            td::Slice alpn = "ton", td::Slice bind_host = "0.0.0.0");
  static td::Result<td::actor::ActorOwn<QuicServer>> create(int port, std::unique_ptr<Callback> callback,
                                                            td::uint64 default_mtu, ServerIdentity identity,
                                                            td::Slice alpn, td::Slice bind_host, Options options);

  td::actor::Task<ServerStats> collect();

 protected:
  void start_up() override;
  void tear_down() override;
  void hangup() override;
  void hangup_shared() override;
  void alarm() override;
  void handle_timeouts();
  void erase_pending_connections();
  void loop() override;

  void notify() override;
  // Requests one loop() pass after the current mailbox drain, without cutting the drain short.
  void schedule_wakeup();

 private:
  friend QuicConnectionPImpl;
  class PImplCallback;

  constexpr static size_t DEFAULT_MTU = 1350;
  constexpr static size_t kMaxBurst = 16;
  constexpr static size_t kIngressBatch = 16;
  constexpr static size_t kEgressBatch = 16;
  constexpr static size_t kMaxDatagram = 64 * 1024;

  struct ConnectionState : td::HeapNode {
    QuicConnectionPImpl &impl() {
      CHECK(impl_);
      return *impl_;
    }
    std::unique_ptr<QuicConnectionPImpl> impl_;
    td::IPAddress remote_address;
    QuicConnectionId cid;
    std::optional<QuicConnectionId> bootstrap_routed_cid;
    std::set<QuicConnectionId> routed_cids;
    bool is_outbound;
    bool in_active_queue = false;
    friend td::StringBuilder &operator<<(td::StringBuilder &sb, const ConnectionState &state) {
      sb << "Connection{" << (state.is_outbound ? "to" : "from") << " " << state.remote_address;
      sb << " cid=" << state.cid;
      sb << "}";
      return sb;
    }
  };
  struct BootstrapRouteKey {
    td::IPAddress remote_address;
    QuicConnectionId routed_cid;
    friend bool operator<(const BootstrapRouteKey &a, const BootstrapRouteKey &b) {
      return std::tie(a.remote_address, a.routed_cid) < std::tie(b.remote_address, b.routed_cid);
    }
  };
  void on_connection_updated(ConnectionState &state);
  void bind_cid(const QuicConnectionId &primary_cid, const QuicConnectionId &cid);
  void unbind_cid(const QuicConnectionId &primary_cid, const QuicConnectionId &cid);
  void unbind_all_cids(ConnectionState &state);
  td::Result<std::shared_ptr<ConnectionState>> install_connection(std::unique_ptr<QuicConnectionPImpl> p_impl,
                                                                  const td::IPAddress &remote_address, bool is_outbound,
                                                                  std::optional<QuicConnectionId> bootstrap_routed_cid);
  void on_local_cid_issued(const QuicConnectionId &primary_cid, const QuicConnectionId &cid);
  void on_local_cid_retired(const QuicConnectionId &primary_cid, const QuicConnectionId &cid);
  // Empty when the datagram was answered statelessly (Retry, invalid-token close) instead of becoming
  // a connection; those answers account for themselves, so this cannot fail.
  std::optional<ServerInitialInfo> prepare_server_initial_info(const VersionCid &initial_packet,
                                                               const td::IPAddress &remote_address);
  td::Result<QuicConnectionId> verify_retry_token(const VersionCid &packet, const td::IPAddress &remote_address) const;
  // Counts and logs its own send failures: a stateless answer we fail to put on the wire is an egress
  // drop, never an invalid inbound datagram.
  void send_stateless_datagram(td::Slice packet_kind, const td::IPAddress &remote_address, td::Slice data);
  // Both answer best effort and account for whatever they could not build, so neither can fail out.
  void send_retry(const VersionCid &packet, const td::IPAddress &remote_address);
  void send_invalid_token_connection_close(const VersionCid &packet, const td::IPAddress &remote_address);

  void update_alarm();
  void drain_ingress();
  void flush_egress();
  bool flush_pending();
  bool produce_next_egress(size_t batch_index);
  void send_connection_close(ConnectionState &state, const UdpMessageBuffer &msg);

  std::shared_ptr<ConnectionState> find_connection(const QuicConnectionId &cid);
  td::Result<std::shared_ptr<ConnectionState>> get_or_create_connection(const UdpMessageBuffer &msg_in);
  td::Result<std::shared_ptr<ConnectionState>> create_inbound_connection(const UdpMessageBuffer &msg_in,
                                                                         const VersionCid &initial_packet,
                                                                         const ServerInitialInfo &initial_info);
  td::Status reject(metrics::Reason reason, td::Status status);
  td::Status ensure_flood_allowed(const std::string &flood_addr);
  void flood_on_inbound_connection_created(const std::string &flood_addr);
  void flood_on_inbound_connection_closed(const std::string &flood_addr);
  QuicConnectionOptions build_connection_options() const;
  bool handle_expiry(ConnectionState &state);
  void log_conn_stats(ConnectionState &state, const char *reason);

  td::UdpSocketFd fd_;
  td::BufferSlice alpn_;
  td::Ref<ServerIdentities> identities_;
  std::array<td::uint8, 32> retry_secret_{};
  Options options_;
  QuicConnectionRateLimiters conn_rate_limiters_;
  adnl::RateLimiter global_conn_rate_limiter_;
  bool gso_enabled_{true};
  bool gro_enabled_{false};
  std::unordered_map<std::string, size_t> flood_map_;

  std::unique_ptr<Callback> callback_;
  td::actor::ActorId<QuicServer> self_id_;

  std::map<QuicConnectionId, QuicConnectionId> cid_to_primary_cid_;
  std::map<BootstrapRouteKey, QuicConnectionId> bootstrap_routes_;
  std::map<QuicConnectionId, std::shared_ptr<ConnectionState>> connections_;
  std::deque<QuicConnectionId> active_connections_;
  std::vector<QuicConnectionId> to_erase_connections_;
  td::KHeap<double> timeout_heap_;

  // Pre-allocated ingress buffers
  std::vector<char> ingress_buffers_;
  std::array<UdpMessageBuffer, kIngressBatch> ingress_msg_;
  std::array<td::UdpSocketFd::InboundMessage, kIngressBatch> ingress_messages_;
  std::array<td::Status, kIngressBatch> ingress_errors_;

  // Pre-allocated egress buffers
  std::array<std::array<char, DEFAULT_MTU * kMaxBurst>, kEgressBatch> egress_buffers_;
  std::array<UdpMessageBuffer, kEgressBatch> egress_batches_;
  std::array<std::shared_ptr<ConnectionState>, kEgressBatch> egress_batch_owners_;
  std::array<td::UdpSocketFd::OutboundMessage, kEgressBatch> egress_messages_;

  // Pending batch state (for handling blocked sends)
  size_t pending_batch_count_ = 0;
  size_t pending_batch_sent_ = 0;

  // Stats
  td::uint64 rx_queue_drops_reflected_ = 0;
  TransportStats transport_stats_;
  QuicConnectionMetricsAggregate closed_conn_stats_;
  metrics::UdpWireStats udp_wire_ = {.dir = {}, .listening_sockets = 1};

  void record_transport_dropped(metrics::Direction dir, metrics::Reason reason) {
    transport_stats_.dropped.at(dir, reason).inc();
  }

  // Wire tier: packet/byte/drop accounting plus exact syscall totals read from the socket.
  void reflect_socket_syscalls();
  void record_egress(td::Span<td::UdpSocketFd::OutboundMessage> batch, const td::UdpSocketFd::SendResult &result);
  void record_ingress(td::Span<td::UdpSocketFd::InboundMessage> batch);

  std::map<adnl::AdnlNodeIdShort, td::uint64> default_mtu_by_local_id_;
  std::map<std::pair<adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort>, td::uint64> peers_mtu_;
};

}  // namespace ton::quic
