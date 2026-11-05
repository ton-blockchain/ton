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
#include "td/utils/HashMap.h"
#include "td/utils/Heap.h"
#include "td/utils/buffer.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/UdpSocketFd.h"

#include "Ed25519.h"
#include "metrics.h"
#include "quic-common.h"
#include "quic-flood-guard.h"  // FloodLimits, CloseCode, StreamOptions, FloodGuard, EgressPicker (re-exported)

namespace ton::quic {
struct QuicConnectionOptions;
struct ServerIdentities;
struct ServerInitialInfo;
struct VersionCid;

struct QuicConnectionPImpl;

class QuicServer : public td::actor::Actor, public td::ObserverBase, private FloodGuard::Host {
 public:
  struct Options {
    bool enable_gso = true;
    bool enable_gro = true;
    bool enable_mmsg = true;
    CongestionControlAlgo cc_algo = CongestionControlAlgo::Bbr;
    std::optional<size_t> max_streams_bidi = std::nullopt;
    std::optional<size_t> max_streams_uni = std::nullopt;
    std::optional<size_t> ack_thresh = std::nullopt;
    std::optional<double> max_ack_delay_seconds = std::nullopt;
    // Bidi-stream credit advertised to the peer on connections WE initiate (outbound). nullopt =
    // use max_streams_bidi. QuicSender sets 0: its outbound connections carry only our own streams.
    std::optional<size_t> outbound_max_peer_streams_bidi = std::nullopt;
    // Uni streams initiated by the peer are also unsolicited on QuicSender's outbound connections.
    std::optional<size_t> outbound_max_peer_streams_uni = std::nullopt;
    bool stateless_retry = true;
    // Messages ride bidirectional streams and confirmation is the peer's empty receipt -- the
    // pre-unidirectional wire behaviour, kept for measurement and old-peer comparison. Consumed by
    // send_message(); nothing outside this server chooses a message's stream kind.
    bool message_streams_bidi = false;
    // RFC 9221 DATAGRAM frame size; 0 keeps messages on streams.
    td::uint32 max_datagram_frame_size = 0;
  };
  // Per-connection peer info, resolved from the (local_id, peer_id) MTU registry at handshake
  // and stamped on the connection: the max message size we accept from the peer, and whether it
  // is trusted to send us heavy traffic.
  struct PeerMtuInfo {
    td::uint64 mtu = 0;
    bool trusted = false;
    bool operator==(const PeerMtuInfo &) const = default;
  };

  class Callback {
   public:
    virtual td::Status on_connected(QuicConnectionId cid, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                    bool is_outbound, PeerMtuInfo peer_info) = 0;
    // A complete admitted message — or the failure that ended the stream — for the application. The
    // FloodGuard reassembles and flood-accounts stream chunks; the app only ever sees whole messages.
    virtual void on_message(QuicConnectionId cid, QuicStreamID sid, td::Result<td::BufferSlice> message) = 0;
    virtual void on_closed(QuicConnectionId cid) = 0;
    // Each peer-initiated unidirectional close must eventually be matched by exactly one
    // release_peer_uni_stream_credit() call, after its payload has been processed.
    virtual void on_stream_closed(QuicConnectionId cid, StreamCloseEvent event) = 0;
    // A live connection's registry entry changed (late registration, trust flip, mtu update).
    virtual void on_peer_info_updated(QuicConnectionId cid, PeerMtuInfo peer_info) {
    }
    virtual ~Callback() = default;
  };

  void send_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data);
  void send_stream_end(QuicConnectionId cid, QuicStreamID sid);
  td::Result<QuicStreamID> open_stream(QuicConnectionId cid, StreamOptions options = {});

  // On error the stream (whether just opened or passed in) is reset — the caller never cleans up.
  td::Result<QuicStreamID> send_stream(QuicConnectionId cid, std::variant<QuicStreamID, StreamOptions> stream,
                                       td::BufferSlice data, bool is_end);

  // Uses a DATAGRAM when enabled, negotiated, and small enough; otherwise a stream. Returns -1 for a DATAGRAM.
  td::Result<QuicStreamID> send_message(QuicConnectionId cid, td::BufferSlice data);

  td::Result<QuicConnectionId> connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key, td::Slice alpn,
                                       td::Slice sni);

  void shutdown_stream(QuicConnectionId cid, QuicStreamID sid);
  // Releases exactly one peer-opened unidirectional stream after the callback has processed it.
  void release_peer_uni_stream_credit(QuicConnectionId cid);
  void on_connection_closed(QuicConnectionId cid);
  // Close a connection, telling the peer why; an abuse close also discourages the IP.
  void close_connection(QuicConnectionId cid, CloseCode code);
  void set_reserved_only(bool reserved_only);
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
  // per-local default. Setting an MTU to 0 erases the corresponding entry; an inbound handshake on a
  // path that resolves to 0 is refused. trusted rides on the peer registration and classifies the
  // connection (untrusted-pooled vs exempt).
  void set_default_mtu(adnl::AdnlNodeIdShort local_id, td::uint64 mtu);
  void set_peer_mtu(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id, td::uint64 mtu, bool trusted);

  // Register an additional ADNL identity on this server. Subsequent identities are reachable via
  // SNI (ServerIdentity::sni(local_id)). Re-registering an id that's already present is a no-op.
  void add_identity(adnl::AdnlNodeIdShort local_id, td::Ed25519::PrivateKey key);

  QuicServer(td::UdpSocketFd fd, td::uint64 default_mtu, ServerIdentity identity, td::BufferSlice alpn,
             std::unique_ptr<Callback> callback, Options options, FloodLimits flood_limits);

  static td::Result<td::actor::ActorOwn<QuicServer>> create(int port, std::unique_ptr<Callback> callback,
                                                            td::uint64 default_mtu, ServerIdentity identity,
                                                            td::Slice alpn = "ton", td::Slice bind_host = "0.0.0.0");
  static td::Result<td::actor::ActorOwn<QuicServer>> create(int port, std::unique_ptr<Callback> callback,
                                                            td::uint64 default_mtu, ServerIdentity identity,
                                                            td::Slice alpn, td::Slice bind_host, Options options,
                                                            FloodLimits flood_limits = {});

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
    // Known after the handshake proves the keys; zero before.
    adnl::AdnlNodeIdShort local_id;
    adnl::AdnlNodeIdShort peer_id;
    PeerMtuInfo peer_info;
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

  // FloodGuard::Host: the transport seam the guard drives (ids only, never sockets/ngtcp2).
  void drop_connection(QuicConnectionId cid, CloseCode code) override;
  void reset_stream(QuicConnectionId cid, QuicStreamID sid) override;
  void deliver(QuicConnectionId cid, QuicStreamID sid, td::Result<td::BufferSlice> message) override;
  void configure(QuicConnectionId cid, FloodGuard::TransportCaps caps) override;
  FloodGuard::IngressGrant grant_ingress(QuicConnectionId cid, td::Timestamp now, td::uint64 cap) override;

  void update_alarm();
  void drain_ingress();
  void flush_egress();
  bool flush_pending();
  size_t produce_next_egress(size_t batch_index);
  std::deque<QuicConnectionId> &active_queue_for(const ConnectionState &state);
  void send_connection_close(ConnectionState &state, const UdpMessageBuffer &msg);
  void send_close_notice(ConnectionState &state, CloseCode code);

  std::shared_ptr<ConnectionState> find_connection(const QuicConnectionId &cid);
  td::Result<QuicStreamID> send_stream_on(ConnectionState &state, std::variant<QuicStreamID, StreamOptions> stream,
                                          td::BufferSlice data, bool is_end);
  td::Result<std::shared_ptr<ConnectionState>> get_or_create_connection(const UdpMessageBuffer &msg_in);
  td::Result<std::shared_ptr<ConnectionState>> create_inbound_connection(const UdpMessageBuffer &msg_in,
                                                                         const VersionCid &initial_packet,
                                                                         const ServerInitialInfo &initial_info);
  td::Status reject(metrics::Reason reason, td::Status status);
  QuicConnectionOptions build_connection_options() const;
  void handle_expiry(ConnectionState &state);
  void log_conn_stats(ConnectionState &state, const char *reason);

  td::UdpSocketFd fd_;
  td::BufferSlice alpn_;
  td::Ref<ServerIdentities> identities_;
  std::array<td::uint8, 32> retry_secret_{};
  Options options_;
  // All flood/DoS policy. Declared before the connection maps so its IngressAggregate outlives
  // every pimpl's IngressShaper, which holds a plain pointer to it.
  FloodGuard guard_;
  EgressPicker picker_;
  bool gso_enabled_{true};
  bool gro_enabled_{false};

  std::unique_ptr<Callback> callback_;
  td::actor::ActorId<QuicServer> self_id_;

  // Hashed, not ordered: both are looked up once per inbound datagram, and an ordered lookup costs a
  // 20-byte memcmp per tree level -- 40 ns against 19 at a thousand connections, 68 against 23 at
  // ten thousand, and it keeps growing while the hashed one does not. Hashing is also what makes the
  // keyed hash necessary: an ordered map has no buckets to flood.
  td::HashMap<QuicConnectionId, QuicConnectionId> cid_to_primary_cid_;
  std::map<BootstrapRouteKey, QuicConnectionId> bootstrap_routes_;
  td::HashMap<QuicConnectionId, std::shared_ptr<ConnectionState>> connections_;
  // Handshake-completed connections indexed by their (local_id, peer_id) so an MTU/trust
  // registry change refreshes only the affected connections, not every connection.
  std::map<adnl::AdnlNodeIdShort, std::map<adnl::AdnlNodeIdShort, std::set<QuicConnectionId>>> conns_by_peer_;
  // Connections with egress to send, split by trust class so the scheduler can prefer trusted in
  // O(1). A connection is in at most one deque (guarded by ConnectionState::in_active_queue).
  std::deque<QuicConnectionId> active_trusted_;
  std::deque<QuicConnectionId> active_untrusted_;
  std::vector<std::pair<QuicConnectionId, bool>> to_erase_connections_;  // (cid, send close notice)
  td::KHeap<double> timeout_heap_;

  // Pre-allocated ingress buffers
  std::vector<char> ingress_buffers_;
  std::array<UdpMessageBuffer, kIngressBatch> ingress_msg_;
  std::array<td::UdpSocketFd::InboundMessage, kIngressBatch> ingress_messages_;
  std::array<td::Status, kIngressBatch> ingress_errors_;

  // Pre-allocated egress buffers
  std::array<std::array<char, DEFAULT_MTU * kMaxBurst>, kEgressBatch> egress_buffers_;
  std::array<UdpMessageBuffer, kEgressBatch> egress_batches_;
  std::array<std::shared_ptr<ConnectionState>, kEgressBatch> egress_message_owners_;
  std::array<td::UdpSocketFd::OutboundMessage, kEgressBatch> egress_messages_;

  // Pending batch state (for handling blocked sends)
  size_t pending_batch_count_ = 0;
  size_t pending_batch_sent_ = 0;

  // Stats
  td::uint64 rx_queue_drops_reflected_ = 0;
  BatchingStats batching_;
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
  std::map<std::pair<adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort>, PeerMtuInfo> peers_mtu_;

  PeerMtuInfo resolve_peer_info(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id) const;
  void refresh_peer_info(ConnectionState &state);
  void refresh_peer_infos(adnl::AdnlNodeIdShort local_id, std::optional<adnl::AdnlNodeIdShort> peer_id);
  // reject_reason is set to `limited` for a local-policy refusal; other errors keep the caller's value.
  td::Status on_handshake_completed(QuicConnectionId cid, td::Slice local_public_key, td::Slice peer_public_key,
                                    bool is_outbound, metrics::Reason &reject_reason);
};

}  // namespace ton::quic
