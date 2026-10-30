#pragma once

#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "adnl/utils.hpp"  // adnl::RateLimiter
#include "td/utils/Heap.h"
#include "td/utils/Status.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"
#include "td/utils/buffer.h"

#include "metrics.h"      // FloodGuardStats
#include "quic-common.h"  // QuicConnectionId, QuicStreamID
#include "quic-connection-rate-limiters.h"
#include "quic-ingress-shaper.h"

namespace ton::quic {

struct StreamOptions {
  std::optional<td::uint64> max_size;               // cap on reassembled bytes; nullopt = unbounded
  td::Timestamp deadline = td::Timestamp::never();  // fail the stream if still pending past this
  // Names the query we sent on this stream, so a limit/deadline failure says what it answered.
  td::uint64 query_size = 0;
  td::int32 query_magic = 0;  // 0 on inbound streams: no query of ours to describe
  // A unidirectional stream has no application receipt or response half to process.
  StreamDirection direction = StreamDirection::Bidirectional;
};

// Application CONNECTION_CLOSE codes (RFC 9000 frame 0x1d): the error space is protocol-defined,
// as in HTTP/3. We borrow conventional HTTP status codes so the peer can log why it was closed
// and pick reconnect behavior without a decoder ring.
enum class CloseCode : td::uint64 {
  ok = 200,
  forbidden = 403,            // policy refusal (reserved-only mode)
  too_many_requests = 429,    // abuse verdict (stream churn) — the IP is being discouraged
  service_unavailable = 503,  // evicted to reclaim capacity — a short refusal, retrying later is fine
};

td::Slice close_code_name(CloseCode code);

// Everything that protects one QuicServer (one listening port) from abusive inbound traffic: the
// single place to read what bounds an untrusted peer. Rationale and defaults: quic/FLOOD-GUARD.md.
struct FloodLimits {
  // Half-open (not yet handshake-completed) inbound connections per IP; excess datagrams are dropped. 0 = unlimited.
  size_t max_handshaking_conns_per_ip = 128;
  // Half-open inbound connections in total; over the cap the oldest is evicted (503, its IP refused for
  // evict_discourage_period). 0 = unlimited.
  size_t max_handshaking_conns = 16384;
  // Max time in the TLS handshake before the connection is closed. 0 = off.
  double handshake_deadline = 5.0;
  // A churn-closed (429) IP may open one connection per throttle period for discourage_period.
  double discourage_period = 30.0;
  double discourage_throttle_period = 5.0;
  // Refusal window after an eviction (503), short so the freed slot is not instantly retaken.
  double evict_discourage_period = 3.0;
  // Refusal window after a reserved-only shed (403). A stronger verdict is never shortened by a weaker one.
  double forbidden_discourage_period = 300.0;
  // Per-peer outbound dial backoff: doubles per failure up to the max, cleared by a successful handshake.
  double dial_backoff_initial = 1.0;
  double dial_backoff_max = 60.0;
  // Shared work-conserving ingress bucket (bytes/s) for untrusted inbound connections. 0 = unshaped.
  td::uint64 untrusted_ingress_rate = td::uint64{50} << 20;
  // Accounted reassembly memory per connection and for the untrusted pool (fattest evicted past the
  // high watermark).
  td::uint64 max_connection_buffered = 256 << 20;
  td::uint64 max_untrusted_buffered = td::uint64{1} << 30;
  // Fixed reassembly charge per live stream, on top of retained payload. 0 = no extra charge.
  td::uint64 stream_state_overhead_bytes = 16 << 10;
  td::uint32 max_untrusted_connections = 4096;  // 0 = unlimited
  // Dispatched untrusted work, charged until the application finishes it. 0 = unlimited.
  td::uint64 max_untrusted_inflight_bytes = td::uint64{1} << 30;
  // Fixed in-flight charge per dispatched request, on top of its payload.
  td::uint64 inflight_request_overhead_bytes = 16 << 10;
  // Separate finite trusted pools: on exhaustion refuse further growth; never evict a trusted
  // connection. 0 = unlimited.
  td::uint64 max_trusted_buffered = td::uint64{4} << 30;
  td::uint64 max_trusted_inflight_bytes = td::uint64{4} << 30;
  // Buffered-egress cap per trusted inbound connection (untrusted uses max_connection_buffered).
  td::uint64 max_trusted_connection_egress = td::uint64{1} << 30;
  // Max time a stream may stay pending (not yet dispatched) before it is failed, by trust class.
  double stream_timeout = 10.0;
  double trusted_stream_timeout = 300.0;
  // An untrusted connection with no application activity for this long is closed (200). 0 = off.
  double untrusted_inactivity_timeout = 120.0;
  // Per-connection bucket of peer stream abandonments; exhausting it closes the connection (429).
  td::uint32 churn_capacity = 64;
  double churn_rate = 64.0;

  // Per-IP live-connection cap; nullopt also disables both new-connection rate limiters
  // (--quic-flood-control -1), but never the discourage table or the half-open caps.
  std::optional<size_t> max_conns_per_ip = 1000;
  td::uint32 conn_rate_capacity = 10;  // per-IP new-connection token bucket
  double conn_rate_period = 0.2;
  td::uint32 global_conn_rate_capacity = 100000;  // Stage-A drops tokenless Initials when drained
  double global_conn_rate_period = 0.00001;

  // Answer cap for the bare send_query (send_query_ex sets its own). Consumed by QuicSender.
  td::uint64 default_max_answer_size = td::uint64{16} << 20;
};

td::StringBuilder &operator<<(td::StringBuilder &sb, const FloodLimits &l);

// Every flood/DoS decision of the QUIC stack. Owned by QuicServer, single-threaded on the server
// actor — no locks. Owns all flood bookkeeping: admission limiters, the discourage table, half-open
// counts and FIFO, the shared ingress bucket and its drain queue, the per-trust-class reassembly and
// in-flight pools, stream reassembly with deadlines, churn buckets and the eviction LRU. Reassembly
// lives here because every buffered byte is flood-accounted. Acts on the transport only through Host
// — it never sees sockets, ngtcp2 or pimpls, only ids.
class FloodGuard {
 public:
  // Per-connection transport knobs the guard decides. Every field is always meaningful — the full
  // triple is (re)applied on each classification event, so applying it is idempotent. Outbound
  // connections always get {max_connection_buffered, false, false}, trust-independent (our own
  // dials are never enforced or shaped).
  struct TransportCaps {
    td::uint64 egress_cap = 0;     // pimpl set_outbound_buffered_cap (0 = unlimited)
    bool egress_enforced = false;  // pimpl set_outbound_cap_enforced; false = log-only
    bool shape_ingress = false;    // pimpl start_ingress_shaping(&ingress_aggregate()) / stop_ingress_shaping
  };
  struct IngressGrant {
    td::uint64 granted = 0;
    td::uint64 debt = 0;  // remaining after the grant; 0 leaves the drain queue
  };

  // The seam to QuicServer: synchronous methods on the server actor. Every method is a no-op /
  // benign-return for an unknown cid — stale cids reach them from re-entrant teardown and the drain.
  class Host {
   public:
    virtual ~Host() = default;
    // Send CONNECTION_CLOSE(code) and tear the connection down (re-enters on_connection_closed).
    virtual void drop_connection(QuicConnectionId cid, CloseCode code) = 0;
    // Reset the transport stream (re-enters on_stream_finished — the app's last word on it).
    virtual void reset_stream(QuicConnectionId cid, QuicStreamID sid) = 0;
    // A complete admitted message — or the failure that ended the stream — for the application.
    virtual void deliver(QuicConnectionId cid, QuicStreamID sid, td::Result<td::BufferSlice> message) = 0;
    // Apply the caps to the pimpl, scheduling a write when they change MAX_DATA.
    virtual void configure(QuicConnectionId cid, TransportCaps caps) = 0;
    // Serve up to cap bytes of the connection's shaped debt (pimpl grant_ingress ->
    // ngtcp2_conn_extend_max_offset), scheduling the MAX_DATA flush. A gone connection returns
    // {0, 0} — indistinguishable from fully served, and correctly handled the same (dequeued).
    virtual IngressGrant grant_ingress(QuicConnectionId cid, td::Timestamp now, td::uint64 cap) = 0;
  };

  FloodGuard(FloodLimits limits, Host &host);
  FloodGuard(const FloodGuard &) = delete;
  ~FloodGuard();

  // build_connection_options reads handshake_deadline and max_connection_buffered; QuicSender
  // reads the client-side knobs.
  const FloodLimits &limits() const {
    return limits_;
  }
  // Handed to pimpl start_ingress_shaping by Host::configure; shaped pimpls keep a plain pointer,
  // so QuicServer declares its guard before its connection maps.
  IngressAggregate &ingress_aggregate() {
    return aggregate_;
  }

  // ---- admission: the raw-ingress path, before any connection state exists ----
  // Stage A (source IP unproven, nothing spent): false = drop a tokenless Initial instead of
  // answering with a Retry we cannot admit.
  bool accepting_handshakes();
  // Stage B (IP proven by retry token): discourage window/throttle, live-per-IP cap, per-IP and
  // global new-connection rates, per-IP half-open cap — in that order, keeping each check's error
  // string verbatim. Error = drop the datagram.
  [[nodiscard]] td::Status admit(const std::string &ip);

  // ---- connection lifecycle ----
  // Count the half-open connection (consuming the discourage throttle slot), start shaping, evict
  // the oldest half-open past the global cap (503).
  void on_inbound_created(QuicConnectionId cid, const std::string &ip);
  // Our own dial: never pooled, shaped or discouraged; egress cap log-only.
  void on_outbound_created(QuicConnectionId cid);
  // The handshake proved the keys: leave the half-open tables, classify into a trust class (pools,
  // shaping, egress cap, churn bucket; per-stream size cap = max_message_size). Error = reserved-only
  // refusal of an untrusted inbound peer: the caller tears the connection down via its normal
  // failed-handshake path (a non-discouraging ok-close), without announcing it to the application.
  [[nodiscard]] td::Status on_handshake_done(QuicConnectionId cid, bool trusted, td::uint64 max_message_size);
  // Registry change on a live connection: re-home pooled bytes, re/un-shape, re-cap.
  void on_peer_info_updated(QuicConnectionId cid, bool trusted, td::uint64 max_message_size);
  // The single deliberate-close path (QuicServer::close_connection delegates here): record the
  // code-graduated discourage verdict — never for a trusted or outbound peer, and a stronger
  // verdict in effect is never shortened — then Host::drop_connection.
  void close_connection(QuicConnectionId cid, CloseCode code);
  // The connection is gone, for any reason. Releases everything it holds; in-flight charges of
  // still-running queries are re-homed until on_stream_finished. Idempotent.
  void on_connection_closed(QuicConnectionId cid);
  // Emergency lever: refuse new untrusted inbound (on_handshake_done errors) and shed existing ones
  // with 403 on the next tick.
  void set_reserved_only(bool reserved_only);

  // ---- streams ----
  // Called at transport stream-open, including streams whose first bytes arrive out of order.
  // Reserves stream metadata and arms its deadline before ngtcp2 retains reordered data.
  [[nodiscard]] td::Status on_peer_stream_open(QuicConnectionId cid, QuicStreamID sid);
  // Reassemble one data event under the per-stream, per-connection and pool budgets; data before
  // successful classification (on_handshake_done) is refused. Stream-open already armed its deadline;
  // data touches the inactivity LRU. A complete message is admitted against the in-flight budget
  // and handed to Host::deliver; its charge is held until the app finishes it, even after transport
  // teardown. On error the failure has already been delivered — the returned status only makes the
  // transport reset the stream, never the connection.
  [[nodiscard]] td::Status on_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data, bool fin);
  // A stream we opened: a never() deadline defaults to trusted_stream_timeout, so a silent peer
  // cannot pin the stream (or our max_streams credit toward it) forever.
  void on_local_stream(QuicConnectionId cid, QuicStreamID sid, StreamOptions options);
  // Transport teardown: orphan an unfinished in-flight charge; an untrusted peer resetting its own
  // stream before dispatch feeds the churn bucket, including a reset before the first byte. A trip
  // queues 429 for tick(); trusted churn is a WARNING, never a close.
  void on_stream_closed(QuicConnectionId cid, StreamCloseEvent event);
  // The app answered or reset the stream: its query is done; release the in-flight charge (directly,
  // or from the orphan table). Tolerates the stream or connection being long gone.
  void on_stream_finished(QuicConnectionId cid, QuicStreamID sid);
  // Egress progress counts as activity: a connection streaming a long answer with no inbound traffic
  // must not be reaped by the inactivity sweep. Called by QuicServer as it produces egress. No-op
  // unless the connection is pooled (untrusted inbound — the only class the sweep touches).
  void note_activity(QuicConnectionId cid);

  // ---- ingress shaping ----
  // Single choke point, right after the transport handled a packet for cid: queue the debtor for the
  // paced drain, dequeue it at zero. Grants happen only in drain_shaper, so every grant is subject to
  // the fair-share cap.
  void on_ingress_debt(QuicConnectionId cid, td::uint64 debt);

  // ---- time ----
  // Stream-deadline sweep (deliver the error, then reset), queued churn/policy closes, byte-budget
  // eviction (fattest pooled first, 503), count-budget and inactivity sweeps (LRU front, 503 / ok),
  // discourage and rate-limiter cleanup. Call from the handle_timeouts slot.
  void tick(td::Timestamp now);
  // Round-robin max-min fair drain of shaped debtors via Host::grant_ingress; internally paced
  // (DRAIN_TICK), a no-op before the pace tick. Call after drain_ingress.
  void drain_shaper(td::Timestamp now);
  // Earliest of: stream deadline, inactivity wake, shaper drain tick, limiter cleanup — or now() while
  // queued closes are pending.
  td::Timestamp next_wakeup() const;

  // ---- observability ----
  // Snapshot for the exporter: the accumulated counters, with every pressure gauge re-read from live
  // state at the moment of the call (they are never accumulated, so a missed collection cannot skew
  // them). Cheap — no iteration over connections.
  FloodGuardStats stats() const;

 private:
  // Move-only RAII debit against a counter exposing charge(n)/release(n). debit() grows the held
  // amount as the resource is consumed; release() — also the destructor — credits the whole
  // outstanding amount, so the counter always reflects exactly what is still held. A moved-from or
  // released token is inert.
  template <class Counter>
  class Charge {
   public:
    Charge() = default;
    explicit Charge(Counter &counter, td::uint64 amount = 0) : counter_(&counter) {
      debit(amount);
    }
    Charge(Charge &&other) noexcept
        : counter_(std::exchange(other.counter_, nullptr)), amount_(std::exchange(other.amount_, 0)) {
    }
    Charge &operator=(Charge &&other) noexcept {
      if (this != &other) {
        release();
        counter_ = std::exchange(other.counter_, nullptr);
        amount_ = std::exchange(other.amount_, 0);
      }
      return *this;
    }
    ~Charge() {
      release();
    }

    void debit(td::uint64 n) {
      if (counter_ && n != 0) {
        counter_->charge(n);
        amount_ += n;
      }
    }
    void release() {
      if (counter_) {
        counter_->release(amount_);
        counter_ = nullptr;
        amount_ = 0;
      }
    }
    td::uint64 amount() const {
      return amount_;
    }

   private:
    Counter *counter_ = nullptr;
    td::uint64 amount_ = 0;
  };

  // A per-server accounting budget (0 = unlimited): retained buffer capacity plus logical stream-state
  // overhead for reassembly, or retained dispatched capacity plus request overhead for in-flight work.
  struct MemoryPool {
    td::uint64 max_bytes = 0;
    td::uint64 buffered = 0;
    void charge(td::uint64 n) {
      buffered += n;
    }
    void release(td::uint64 n) {
      CHECK(n <= buffered);
      buffered -= n;
    }
    bool would_exceed(td::uint64 n) const {
      return max_bytes != 0 && (buffered > max_bytes || n > max_bytes - buffered);
    }
    // Once over the 90% high watermark, how much to reclaim to fall back to the 75% low watermark.
    // Refusal alone can wedge the pool at capacity for a whole query lifetime, so past the high
    // watermark we shed the fattest holders instead of only refusing newcomers.
    td::uint64 bytes_to_reclaim() const {
      if (max_bytes == 0 || buffered <= max_bytes / 10 * 9) {
        return 0;  // 0 = unlimited: never reclaim (matches would_exceed treating 0 as no cap)
      }
      return buffered - max_bytes / 4 * 3;
    }
  };

  class ConnectionMemory;  // per-connection reassembly accounting (defined in the .cpp)
  class PoolMembership;    // RAII pool byte-contribution + eviction-LRU node (defined in the .cpp)
  struct StreamState;      // intrusive-heap reassembly buffer + charges (defined in the .cpp)
  struct Conn;             // per-connection flood record (defined in the .cpp)
  struct Discouraged {
    td::Timestamp until;
    td::Timestamp next_allowed;  // strict throttle: one admission per throttle period
  };
  struct StreamLookup {
    StreamState *state;
    bool inserted;
    Conn *conn;
  };

  Conn *find_conn(QuicConnectionId cid);
  void leave_half_open(Conn &conn);
  void dequeue_shaper(Conn &conn);
  TransportCaps compute_caps(const Conn &conn) const;
  void apply_caps(QuicConnectionId cid, Conn &conn);

  // Inbound connections are pooled by trust class; our own outbound connections are unpooled.
  static bool is_untrusted_inbound(const Conn &conn);
  static bool is_trusted_inbound(const Conn &conn);
  MemoryPool *inflight_pool_for(const Conn &conn);
  MemoryPool *buffered_pool_for(const Conn &conn);
  void maybe_warn_trusted_buffered();
  void refresh_pool_membership(QuicConnectionId cid, Conn &conn);

  td::Result<StreamLookup> get_or_create_stream(QuicConnectionId cid, QuicStreamID sid);
  void orphan_unfinished_inflight(QuicConnectionId cid, QuicStreamID sid, StreamState &state);
  void apply_stream_options(StreamState &state, const StreamOptions &options);
  void fail_stream(StreamState &state, StreamFail reason, td::Status error);

  void evict_over_byte_budget();
  void evict_over_count_budget();
  void close_inactive_untrusted(td::Timestamp now);
  bool has_outstanding_work(QuicConnectionId cid, const Conn &conn) const;
  td::Timestamp inactivity_deadline(td::Timestamp last_activity) const;

  FloodLimits limits_;
  Host &host_;

  // admission
  QuicConnectionRateLimiters conn_rate_;
  adnl::RateLimiter global_conn_rate_;
  std::unordered_map<std::string, size_t> live_by_ip_;
  std::unordered_map<std::string, size_t> half_open_by_ip_;
  std::list<QuicConnectionId> half_open_fifo_;  // oldest at front; the global cap evicts from here
  std::unordered_map<std::string, Discouraged> discouraged_;

  // shaping: the shared bucket lives here; the per-connection debt ledger stays in the pimpl's
  // IngressShaper, which holds a plain pointer to aggregate_.
  IngressAggregate aggregate_;
  std::list<QuicConnectionId> shaper_waiters_;
  td::Timestamp next_shaper_drain_;

  // memory & streams: pools declared before conns_, so stream teardown releases into live counters.
  // Trusted and untrusted have independent reassembly/in-flight pools (neither class can squeeze the
  // other); the trusted reassembly pool has no eviction LRU (refuse further growth, never shed).
  MemoryPool untrusted_pool_, untrusted_inflight_, trusted_pool_, trusted_inflight_;
  bool trusted_pool_warned_ = false;           // one WARNING per crossing of the soft watermark
  std::list<QuicConnectionId> untrusted_lru_;  // pooled untrusted conns, least recently active first
  std::map<QuicConnectionId, std::unique_ptr<Conn>> conns_;
  // In-flight charges of streams the transport tore down mid-query: the query still runs, so each
  // charge lives here until the app finishes its stream.
  std::map<std::pair<QuicConnectionId, QuicStreamID>, Charge<MemoryPool>> orphaned_inflight_;
  std::vector<std::pair<QuicConnectionId, CloseCode>> pending_closes_;  // churn / reserved sheds
  td::KHeap<double> stream_deadlines_;
  bool reserved_only_ = false;

  // Counters only; stats() fills the gauges from live state.
  FloodGuardStats stats_;
  void count_stream_failure(StreamFail reason) {
    stats_.streams_failed.at(reason).inc();
  }
};

// Trusted-first egress pick order with anti-starvation (4:1 at weight 5). The active queues stay
// with QuicServer's batching machinery; only the policy lives here. Advance on served picks only,
// so skips don't skew the ratio.
class EgressPicker {
 public:
  static constexpr size_t kTrustedWeight = 5;
  bool prefer_untrusted() const {
    return served_ % kTrustedWeight == kTrustedWeight - 1;
  }
  void on_served() {
    served_++;
  }

 private:
  td::uint64 served_ = 0;
};

// Client-side per-key dial backoff: while backed off, check() fails fast with the failure that
// armed it; the delay doubles per failure up to the max and only a successful handshake clears it.
// Lives on the QuicSender actor — a DIFFERENT actor from FloodGuard — so it is a plain value
// sharing the FloodLimits knobs but never state. Key = QuicSender::AdnlPath.
template <class Key>
class DialBackoff {
 public:
  DialBackoff(double initial_delay, double max_delay) : initial_delay_(initial_delay), max_delay_(max_delay) {
  }
  td::Status check(const Key &key) const {
    if (auto it = entries_.find(key); it != entries_.end() && !it->second.next_dial_after.is_in_past()) {
      return it->second.last_error.clone();
    }
    return td::Status::OK();
  }
  void on_failure(const Key &key, const td::Status &error) {
    auto &e = entries_[key];
    e.delay = e.delay == 0.0 ? initial_delay_ : std::min(e.delay * 2.0, max_delay_);
    e.next_dial_after = td::Timestamp::in(e.delay);
    e.last_error = error.clone();
  }
  void on_success(const Key &key) {
    entries_.erase(key);
  }
  // Drop entries idle for max_delay past their window, so a dead peer keeps its grown delay across gc.
  void gc() {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (td::Timestamp::in(max_delay_, it->second.next_dial_after).is_in_past()) {
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  }

 private:
  struct Entry {
    td::Timestamp next_dial_after;
    double delay = 0.0;
    td::Status last_error;
  };
  double initial_delay_;
  double max_delay_;
  std::map<Key, Entry> entries_;
};

}  // namespace ton::quic
