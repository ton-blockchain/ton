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
#include "td/actor/ActorOwn.h"
#include "td/actor/core/Actor.h"
#include "td/utils/Heap.h"
#include "td/utils/buffer.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/UdpSocketFd.h"

#include "Ed25519.h"
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
    // Bidi-stream credit advertised to the peer on connections WE initiate (outbound). nullopt =
    // use max_streams_bidi. QuicSender sets 0: its outbound connections carry only our own streams.
    std::optional<size_t> outbound_max_peer_streams_bidi = std::nullopt;
    bool stateless_retry = true;
  };
  // Per-connection peer info, resolved from the (local_id, peer_id) MTU registry at handshake
  // and stamped on the connection: the max message size we accept from the peer, and whether it
  // is trusted to send us heavy traffic.
  struct PeerMtuInfo {
    td::uint64 mtu = 0;
    bool trusted = false;
  };

  class Callback {
   public:
    virtual td::Status on_connected(QuicConnectionId cid, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                    bool is_outbound, PeerMtuInfo peer_info) = 0;
    // A complete admitted message — or the failure that ended the stream — for the application. The
    // FloodGuard reassembles and flood-accounts stream chunks; the app only ever sees whole messages.
    virtual void on_message(QuicConnectionId cid, QuicStreamID sid, td::Result<td::BufferSlice> message) = 0;
    virtual void on_closed(QuicConnectionId cid) = 0;
    virtual void on_stream_closed(QuicConnectionId cid, QuicStreamID sid) = 0;
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

  td::Result<QuicConnectionId> connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key, td::Slice alpn,
                                       td::Slice sni);

  void shutdown_stream(QuicConnectionId cid, QuicStreamID sid);
  void on_connection_closed(QuicConnectionId cid);
  // Close a connection, telling the peer why; an abuse close also discourages the IP.
  void close_connection(QuicConnectionId cid, CloseCode code);
  void set_reserved_only(bool reserved_only);
  void log_stats(std::string reason = "stats");

  // MTU state is keyed by local_id and (local_id, peer_id). Peer-specific MTU overrides the
  // per-local default. Setting an MTU to 0 erases the corresponding entry. trusted rides on the
  // peer registration and classifies the connection (untrusted-pooled vs exempt).
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

  struct Stats {
    struct Entry {
      size_t total_conns = 1;
      QuicConnectionStats impl_stats = {};

      Entry operator+(const Entry &other) const {
        Entry res = {.total_conns = total_conns + other.total_conns, .impl_stats = impl_stats + other.impl_stats};
        auto tc = total_conns + other.total_conns;
        if (tc > 0)
          res.impl_stats.mean_rtt = (static_cast<double>(total_conns) * impl_stats.mean_rtt +
                                     static_cast<double>(other.total_conns) * other.impl_stats.mean_rtt) /
                                    static_cast<double>(tc);
        return res;
      }
    };

    Entry summary = {.total_conns = 0};
    std::unordered_map<QuicConnectionId, Entry> per_conn = {};
  };

  void collect_stats(td::Promise<Stats> P);

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

 private:
  friend QuicConnectionPImpl;
  class PImplCallback;

  constexpr static size_t DEFAULT_MTU = 1350;
  constexpr static size_t kMaxBurst = 16;
  constexpr static size_t kIngressBatch = 16;
  constexpr static size_t kEgressBatch = 16;
  constexpr static size_t kMaxDatagram = 64 * 1024;
  constexpr static td::uint64 kInboundDropLogEvery = 1 << 14;

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
  td::Result<std::optional<ServerInitialInfo>> prepare_server_initial_info(const VersionCid &initial_packet,
                                                                           const td::IPAddress &remote_address);
  td::Result<QuicConnectionId> verify_retry_token(const VersionCid &packet, const td::IPAddress &remote_address) const;
  td::Status send_stateless_datagram(td::Slice packet_kind, const td::IPAddress &remote_address, td::Slice data);
  td::Status send_retry(const VersionCid &packet, const td::IPAddress &remote_address);
  td::Status send_invalid_token_connection_close(const VersionCid &packet, const td::IPAddress &remote_address);

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
  bool produce_next_egress(size_t batch_index);
  std::deque<QuicConnectionId> &active_queue_for(const ConnectionState &state);
  void send_connection_close(ConnectionState &state, const UdpMessageBuffer &msg);
  void send_close_notice(ConnectionState &state, CloseCode code);

  std::shared_ptr<ConnectionState> find_connection(const QuicConnectionId &cid);
  td::Result<std::shared_ptr<ConnectionState>> get_or_create_connection(const UdpMessageBuffer &msg_in);
  QuicConnectionOptions build_connection_options() const;
  bool handle_expiry(ConnectionState &state);
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

  std::map<QuicConnectionId, QuicConnectionId> cid_to_primary_cid_;
  std::map<BootstrapRouteKey, QuicConnectionId> bootstrap_routes_;
  std::map<QuicConnectionId, std::shared_ptr<ConnectionState>> connections_;
  // Handshake-completed connections indexed by their (local_id, peer_id) so an MTU/trust
  // registry change refreshes only the affected connections, not every connection.
  std::map<adnl::AdnlNodeIdShort, std::map<adnl::AdnlNodeIdShort, std::set<QuicConnectionId>>> conns_by_peer_;
  // Connections with egress to send, split by trust class so the scheduler can prefer trusted in
  // O(1). A connection is in at most one deque (guarded by ConnectionState::in_active_queue).
  std::deque<QuicConnectionId> active_trusted_;
  std::deque<QuicConnectionId> active_untrusted_;
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

  // UDP-level stats
  struct UdpStats {
    td::uint64 syscalls = 0;
    td::uint64 packets = 0;
    td::uint64 bytes = 0;
  };
  UdpStats ingress_stats_;
  UdpStats egress_stats_;
  td::uint64 inbound_drop_log_counter_ = 0;

  std::map<adnl::AdnlNodeIdShort, td::uint64> default_mtu_by_local_id_;
  std::map<std::pair<adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort>, PeerMtuInfo> peers_mtu_;

  PeerMtuInfo resolve_peer_info(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id) const;
  void refresh_peer_info(ConnectionState &state);
  void refresh_peer_infos(adnl::AdnlNodeIdShort local_id, std::optional<adnl::AdnlNodeIdShort> peer_id);
  td::Status on_handshake_completed(QuicConnectionId cid, td::Slice local_public_key, td::Slice peer_public_key,
                                    bool is_outbound);
};

}  // namespace ton::quic
