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

#include "quic-common.h"  // QuicConnectionId, QuicStreamID
#include "quic-connection-rate-limiters.h"
#include "quic-ingress-shaper.h"

namespace ton::quic {

struct StreamOptions {
  std::optional<td::uint64> max_size;               // cap on reassembled bytes; nullopt = unbounded
  td::Timestamp deadline = td::Timestamp::never();  // fail the stream if still pending past this
};

// Application CONNECTION_CLOSE codes (RFC 9000 frame 0x1d): the error space is protocol-defined,
// as in HTTP/3. We borrow conventional HTTP status codes so the peer can log why it was closed
// and pick reconnect behavior without a decoder ring.
enum class CloseCode : td::uint64 {
  ok = 200,
  forbidden = 403,            // policy refusal (reserved-only mode)
  too_many_requests = 429,    // abuse verdict (stream churn) — the IP is being discouraged
  service_unavailable = 503,  // evicted to reclaim capacity — retrying later is fine
};

td::Slice close_code_name(CloseCode code);

// Everything that protects one QuicServer (one listening port) from abusive inbound traffic: the
// half-open/handshake caps, per-connection and per-untrusted-pool reassembly memory, the in-flight
// query budget, ingress/egress shaping, and the churn/discourage levers. The single place to read
// what bounds an untrusted peer.
struct FloodLimits {
  // Bound half-open (installed but not-yet-handshake-completed) inbound connections per IP: further
  // datagrams from an IP already at the cap are dropped. 0 = unlimited.
  size_t max_handshaking_conns_per_ip = 128;
  // Global bound on half-open inbound connections across all IPs. When over the cap the OLDEST
  // half-open connection is evicted (503, not discouraged — it may retry at once), never the
  // newcomer: refusing would let an attacker holding the table lock out honest peers, whereas an
  // honest handshake only has to survive a round trip. 0 = unlimited. Generous (~16k half-open ≈
  // a few hundred MB of transient ngtcp2/TLS state): eviction only bites under a genuine flood, and
  // the per-IP cap (128) plus the 5s handshake deadline already bound any single source.
  size_t max_handshaking_conns = 16384;
  // Max time a connection may stay in the TLS handshake before it is closed. Bounds standing
  // half-open memory to ~(new-connection rate x this); tighter than the 15s idle timeout. 0 = off.
  double handshake_deadline = 5.0;
  // A discouraged (churn-closed) IP is throttled, not refused outright: at most one new connection
  // per throttle period for discourage_period. Absolute refusal would let one abuser behind a NAT
  // lock out a trusted peer on the same IP (trust is only provable post-handshake).
  double discourage_period = 30.0;
  double discourage_throttle_period = 5.0;
  // Discourage duration is graduated by close reason: churn (429) uses discourage_period; a capacity
  // eviction (503) a short window so a freed slot isn't instantly retaken; a policy refusal (403,
  // reserved-only) a long one. A stronger verdict never shortens a weaker one already in effect.
  double evict_discourage_period = 3.0;
  double forbidden_discourage_period = 300.0;
  // Client-side per-peer dial backoff: after a failed outbound dial, wait before re-dialing, doubling
  // up to the max, reset on a successful handshake. Stops us hammering a persistently broken peer.
  double dial_backoff_initial = 1.0;
  double dial_backoff_max = 60.0;
  // Aggregate ingress rate (bytes/s) for this server's untrusted inbound connections, drawn from one
  // shared work-conserving token bucket (an active connection uses the full rate when others idle).
  // 0 = disabled (line-rate credit). Untrusted full nodes send legitimate block broadcasts and share
  // this budget, but their steady-state aggregate is well under this — 50 MiB/s (400 Mbit/s) is a
  // real cap against a flood while leaving honest broadcast propagation untouched.
  td::uint64 untrusted_ingress_rate = td::uint64{50} << 20;
  // Reassembly memory: each connection is bounded on its own, and all untrusted inbound connections
  // share a per-server pool (the fattest are evicted once it crosses the high watermark).
  td::uint64 max_connection_buffered = 256 << 20;
  td::uint64 max_untrusted_buffered = td::uint64{1} << 30;
  td::uint32 max_untrusted_connections = 4096;  // 0 = unlimited
  // Untrusted dispatched payload, bounded by a per-server in-flight pool. Once a complete message
  // is dispatched it is non-reclaimable — it runs to completion downstream — so it is capped only
  // at admission (checked at dispatch, next to the buffer limits); the stream holds the charge
  // until it closes, i.e. for the life of the query it carries. Trusted peers and our own outbound
  // streams are exempt. 0 = unlimited.
  td::uint64 max_untrusted_inflight_bytes = td::uint64{1} << 30;
  // Trusted peers are not exempt, only far more generously bounded — enough that legitimate
  // validator traffic never touches these, but finite so a single compromised validator key can't
  // exhaust memory. Hitting any of them is a compromise/bug signal (logged loudly). Their pools are
  // independent of the untrusted ones (neither class can squeeze the other), and the trusted
  // reassembly pool is never eviction-shed (fail the newest stream instead — never drop a validator
  // mid-broadcast). 0 = unlimited.
  td::uint64 max_trusted_buffered = td::uint64{4} << 30;
  td::uint64 max_trusted_inflight_bytes = td::uint64{4} << 30;
  // Enforced buffered-egress cap per trusted connection (untrusted uses max_connection_buffered = 256 MB).
  td::uint64 max_trusted_connection_egress = td::uint64{1} << 30;
  // Max time a peer-initiated stream may stay pending (reassembling / waiting to be handled) before
  // it is failed. Only pending streams are cancellable; a dispatched query runs to completion.
  // Untrusted peers get the tight bound (a broadcast block arrives in <200ms; a slow one is stale);
  // trusted peers get a far more generous one for legitimate slow/large transfers, but are still
  // bounded so a stuck stream can't pin reassembly state forever.
  double stream_timeout = 10.0;
  double trusted_stream_timeout = 300.0;
  // An untrusted connection kept alive but with no stream activity for this long is closed
  // (CloseCode::ok) to reclaim its per-IP/table slots. The QUIC idle timeout only reaps truly
  // silent connections, and our own keep-alive PINGs keep a request-idle peer alive forever, so an
  // app-level backstop is needed. Trusted peers are never inactivity-closed. 0 = off.
  double untrusted_inactivity_timeout = 120.0;
  // Per-connection stream-churn bucket. A peer that repeatedly opens a stream, dribbles a few bytes,
  // and abandons it (reset before FIN) recycles wire buffers behind the reassembly cap. Absorb
  // churn_capacity such abandonments, refilling churn_rate/s; the next one trips the connection
  // (closed, its IP discouraged). Deadline/eviction kills never count as churn.
  td::uint32 churn_capacity = 64;
  double churn_rate = 64.0;

  // Admission, per source IP (moved from QuicServer::Options). nullopt disables the live-
  // connection cap and both new-connection rate limiters (--quic-flood-control -1) — but never
  // the discourage table, and never the per-IP half-open cap (its own knob above).
  std::optional<size_t> max_conns_per_ip = 1000;
  td::uint32 conn_rate_capacity = 10;  // per-IP new-connection token bucket
  double conn_rate_period = 0.2;
  td::uint32 global_conn_rate_capacity = 100000;  // Stage-A drops tokenless Initials when drained
  double global_conn_rate_period = 0.00001;

  // Default answer cap for the bare send_query (send_query_ex sets its own). Consumed by QuicSender.
  // Generous — every large transfer (block/proof/state downloads, overlay queries) already goes
  // through send_query_ex with an explicit size — but finite, so a bare query can't be answered with
  // an unbounded stream that pins reassembly memory up to the per-connection buffer cap.
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
  bool accepting_handshakes() const;
  // Stage B (IP proven by retry token): discourage window/throttle, live-per-IP cap, per-IP and
  // global new-connection rates, per-IP half-open cap — in that order, keeping each check's error
  // string verbatim. Error = drop the datagram.
  td::Status admit(const std::string &ip);

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
  // Reassemble one data event under the per-stream, per-connection and pool budgets; data before
  // successful classification (on_handshake_done) is refused. First data arms the trust-class
  // deadline and touches the inactivity LRU. A complete message is admitted against the in-flight
  // budget and handed to Host::deliver; its charge is held until the stream closes AND the app
  // finishes it. On error the failure has already been delivered — the returned status only makes the
  // transport reset the stream, never the connection.
  [[nodiscard]] td::Status on_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data, bool fin);
  // A stream we opened: a never() deadline defaults to trusted_stream_timeout, so a silent peer
  // cannot pin the stream (or our max_streams credit toward it) forever.
  void on_local_stream(QuicConnectionId cid, QuicStreamID sid, StreamOptions options);
  // Transport teardown: orphan an unfinished in-flight charge; an untrusted peer abandoning its own
  // still-buffering stream feeds the churn bucket — a trip queues 429 for tick(); trusted churn is a
  // WARNING, never a close.
  void on_stream_closed(QuicConnectionId cid, QuicStreamID sid);
  // The app answered or reset the stream: its query is done; release the in-flight charge (directly,
  // or from the orphan table). Tolerates the stream or connection being long gone.
  void on_stream_finished(QuicConnectionId cid, QuicStreamID sid);
  // Egress progress counts as activity: a connection streaming a long answer with no inbound traffic
  // must not be reaped by the inactivity sweep. Called by QuicServer as it produces egress. No-op
  // unless the connection is pooled (untrusted inbound — the only class the sweep touches).
  void note_activity(QuicConnectionId cid);

  // ---- ingress shaping ----
  // Single choke point, right after the transport handled a packet for cid (replaces
  // service_shaper): queue the debtor for the paced drain, dequeue it at zero. Grants happen only
  // in drain_shaper, so every grant is subject to the fair-share cap.
  void on_ingress_debt(QuicConnectionId cid, td::uint64 debt);

  // ---- time ----
  // Stream-deadline sweep (deliver the error, then reset), queued churn/policy closes, byte-budget
  // eviction (fattest pooled first, 503), count-budget and inactivity sweeps (LRU front, 503 / ok),
  // discourage and rate-limiter cleanup. Call from the handle_timeouts slot.
  void tick(td::Timestamp now);
  // Round-robin max-min fair drain of shaped debtors via Host::grant_ingress; internally paced
  // (DRAIN_TICK), a no-op before the pace tick. Call from today's drain_shaper slot, after
  // drain_ingress.
  void drain_shaper(td::Timestamp now);
  // Earliest of: stream deadline, inactivity wake, shaper drain tick, limiter cleanup — or now() while
  // queued closes are pending.
  td::Timestamp next_wakeup() const;

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

  // A per-server byte budget (0 = unlimited): reassembly bytes of all untrusted inbound connections,
  // or their dispatched payload (the in-flight pool).
  struct MemoryPool {
    td::uint64 max_bytes = 0;
    td::uint64 buffered = 0;
    void charge(td::uint64 n) {
      buffered += n;
    }
    void release(td::uint64 n) {
      buffered -= n;
    }
    bool would_exceed(td::uint64 n) const {
      return max_bytes != 0 && buffered + n > max_bytes;
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
  void fail_stream(StreamState &state, td::Status error);

  void evict_over_byte_budget();
  void evict_over_count_budget();
  void close_inactive_untrusted(td::Timestamp now);
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
  // other); the trusted reassembly pool has no eviction LRU (fail the newest stream, never shed).
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
  // Drop entries whose backoff window has elapsed (check() already returns OK for them); a later
  // failure re-creates the entry from the initial delay. Bounds the map to actively-backing-off
  // paths, so a node that dials many never-reachable peers over a long uptime doesn't grow it without
  // bound. Call periodically.
  void gc() {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.next_dial_after.is_in_past()) {
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
