#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "td/actor/ActorOwn.h"
#include "td/actor/core/Actor.h"
#include "td/utils/Heap.h"
#include "td/utils/buffer.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/UdpSocketFd.h"

#include "Ed25519.h"
#include "quic-common.h"

namespace ton::quic {
struct QuicConnectionPImpl;

struct StreamOptions {
  std::optional<td::uint64> max_size;
  td::Timestamp timeout = td::Timestamp::never();
  double timeout_seconds = 0.0;
  td::uint64 query_size = 0;
  td::uint32 query_magic = 0;
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
  };
  class Callback {
   public:
    virtual void on_connected(QuicConnectionId cid, td::SecureString peer_public_key, bool is_outbound) = 0;
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
    virtual ~Callback() = default;
  };

  void send_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data);
  void send_stream_end(QuicConnectionId cid, QuicStreamID sid);
  td::Result<QuicStreamID> open_stream(QuicConnectionId cid, StreamOptions options = {});

  td::Result<QuicStreamID> send_stream(QuicConnectionId cid, std::variant<QuicStreamID, StreamOptions> stream,
                                       td::BufferSlice data, bool is_end);
  void change_stream_options(QuicConnectionId cid, QuicStreamID sid, StreamOptions options);

  td::Result<QuicConnectionId> connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key, td::Slice alpn);

  void shutdown_stream(QuicConnectionId cid, QuicStreamID sid);
  void close(QuicConnectionId cid);
  void log_stats(std::string reason = "stats");

  constexpr static size_t DEFAULT_FLOOD_CONTROL = 10;

  QuicServer(td::UdpSocketFd fd, td::Ed25519::PrivateKey server_key, td::BufferSlice alpn,
             std::unique_ptr<Callback> callback, Options options,
             std::optional<size_t> flood_control = DEFAULT_FLOOD_CONTROL);

  static td::Result<td::actor::ActorOwn<QuicServer>> create(int port, td::Ed25519::PrivateKey server_key,
                                                            std::unique_ptr<Callback> callback, td::Slice alpn = "ton",
                                                            td::Slice bind_host = "0.0.0.0");
  static td::Result<td::actor::ActorOwn<QuicServer>> create(int port, td::Ed25519::PrivateKey server_key,
                                                            std::unique_ptr<Callback> callback, td::Slice alpn,
                                                            td::Slice bind_host, Options options);

  struct Stats {
    struct Entry {
      size_t total_conns = 1;
      QuicConnectionStats impl_stats = {};

      Entry operator+(const Entry &other) const {
        Entry res = {.total_conns = total_conns + other.total_conns, .impl_stats = impl_stats + other.impl_stats};
        res.impl_stats.mean_rtt = (total_conns * impl_stats.mean_rtt + other.total_conns * other.impl_stats.mean_rtt) /
                                  (total_conns + other.total_conns);
        return res;
      }

      Entry operator-(const Entry &other) const {
        Entry res = {.total_conns = total_conns - other.total_conns, .impl_stats = impl_stats - other.impl_stats};
        res.impl_stats.mean_rtt = impl_stats.mean_rtt;
        return res;
      }

    } summary = {};
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

  struct ConnectionState : td::HeapNode {
    QuicConnectionPImpl &impl() {
      CHECK(impl_);
      return *impl_;
    }
    std::unique_ptr<QuicConnectionPImpl> impl_;
    td::IPAddress remote_address;
    QuicConnectionId cid;
    std::optional<QuicConnectionId> temp_cid;
    bool is_outbound;
    bool in_active_queue = false;
    friend td::StringBuilder &operator<<(td::StringBuilder &sb, const ConnectionState &state) {
      sb << "Connection{" << (state.is_outbound ? "to" : "from") << " " << state.remote_address;
      sb << " cid=" << state.cid;
      if (state.temp_cid) {
        sb << " (temp=" << state.temp_cid.value() << ")";
      }
      sb << "}";
      return sb;
    }
  };
  void on_connection_updated(ConnectionState &state);

  void update_alarm();
  void drain_ingress();
  void flush_egress();
  bool flush_pending();
  bool produce_next_egress(size_t batch_index);

  std::shared_ptr<ConnectionState> find_connection(const QuicConnectionId &cid);
  td::Result<std::shared_ptr<ConnectionState>> get_or_create_connection(const UdpMessageBuffer &msg_in);
  bool handle_expiry(ConnectionState &state);
  void log_conn_stats(ConnectionState &state, const char *reason);

  td::UdpSocketFd fd_;
  td::BufferSlice alpn_;
  td::Ed25519::PrivateKey server_key_;
  bool gso_enabled_{true};
  bool gro_enabled_{false};
  CongestionControlAlgo cc_algo_{CongestionControlAlgo::Cubic};
  std::optional<size_t> flood_control_;
  std::unordered_map<std::string, size_t> flood_map_;

  std::unique_ptr<Callback> callback_;
  td::actor::ActorId<QuicServer> self_id_;

  std::map<QuicConnectionId, QuicConnectionId> to_primary_cid_;
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

  // UDP-level stats
  struct UdpStats {
    td::uint64 syscalls = 0;
    td::uint64 packets = 0;
    td::uint64 bytes = 0;
  };
  UdpStats ingress_stats_;
  UdpStats egress_stats_;
};

}  // namespace ton::quic
