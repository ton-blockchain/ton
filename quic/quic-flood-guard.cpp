#include <algorithm>

#include "td/utils/algorithm.h"
#include "td/utils/logging.h"

#include "quic-flood-guard.h"

namespace ton::quic {

// QUIC stream ids carry the initiator in bit 0: 0 = client, 1 = server.
static bool is_peer_initiated(bool is_outbound, QuicStreamID sid) {
  // Today this always equals !is_outbound: outbound connections grant the peer zero bidi-stream
  // credit and we never open streams on inbound ones. But that rests on config and usage, not the
  // wire format — no CHECK, it would become remotely trippable if the credit pin is ever lifted.
  bool server_initiated = (sid & 1) != 0;
  return server_initiated == is_outbound;
}

// Lock the armed default so a change is deliberate — and prompts a review of quic-ddos-protection.md.
static_assert(FloodLimits{}.untrusted_ingress_rate == (td::uint64{50} << 20),
              "untrusted_ingress_rate default changed; re-check the DDoS doc and soak plan");

td::StringBuilder &operator<<(td::StringBuilder &sb, const FloodLimits &l) {
  sb << "max_handshaking_conns_per_ip=" << l.max_handshaking_conns_per_ip
     << " max_handshaking_conns=" << l.max_handshaking_conns << " handshake_deadline=" << l.handshake_deadline
     << " discourage_period=" << l.discourage_period << " discourage_throttle_period=" << l.discourage_throttle_period
     << " evict_discourage_period=" << l.evict_discourage_period
     << " forbidden_discourage_period=" << l.forbidden_discourage_period
     << " dial_backoff_initial=" << l.dial_backoff_initial << " dial_backoff_max=" << l.dial_backoff_max
     << " untrusted_ingress_rate=" << l.untrusted_ingress_rate
     << " max_connection_buffered=" << l.max_connection_buffered
     << " max_untrusted_buffered=" << l.max_untrusted_buffered
     << " max_untrusted_connections=" << l.max_untrusted_connections
     << " max_untrusted_inflight_bytes=" << l.max_untrusted_inflight_bytes
     << " max_trusted_buffered=" << l.max_trusted_buffered
     << " max_trusted_inflight_bytes=" << l.max_trusted_inflight_bytes
     << " max_trusted_connection_egress=" << l.max_trusted_connection_egress << " stream_timeout=" << l.stream_timeout
     << " trusted_stream_timeout=" << l.trusted_stream_timeout
     << " untrusted_inactivity_timeout=" << l.untrusted_inactivity_timeout << " churn_capacity=" << l.churn_capacity
     << " churn_rate=" << l.churn_rate << " max_conns_per_ip=";
  if (l.max_conns_per_ip) {
    sb << *l.max_conns_per_ip;
  } else {
    sb << "off";
  }
  return sb << " conn_rate_capacity=" << l.conn_rate_capacity << " conn_rate_period=" << l.conn_rate_period
            << " global_conn_rate_capacity=" << l.global_conn_rate_capacity
            << " global_conn_rate_period=" << l.global_conn_rate_period
            << " default_max_answer_size=" << l.default_max_answer_size;
}

td::Slice close_code_name(CloseCode code) {
  switch (code) {
    case CloseCode::ok:
      return "OK";
    case CloseCode::forbidden:
      return "Forbidden";
    case CloseCode::too_many_requests:
      return "Too Many Requests";
    case CloseCode::service_unavailable:
      return "Service Unavailable";
  }
  return "unknown";
}

// How long to discourage the peer's IP after we close it with this code (0 = not at all).
static double discourage_period_for(const FloodLimits &limits, CloseCode code) {
  switch (code) {
    case CloseCode::too_many_requests:
      return limits.discourage_period;
    case CloseCode::service_unavailable:
      return limits.evict_discourage_period;
    case CloseCode::forbidden:
      return limits.forbidden_discourage_period;
    case CloseCode::ok:
      return 0.0;
  }
  return 0.0;
}

// Per-connection reassembly accounting. Charges its own counter and, while the connection belongs to
// the untrusted pool, the shared pool too. set_pool moves the held bytes when the connection's tier
// flips; the two counters can never drift apart.
class FloodGuard::ConnectionMemory {
 public:
  // Join/leave the untrusted byte pool, moving the held bytes with it. Driven only by
  // PoolMembership, which owns the count/LRU node alongside this byte contribution.
  void set_pool(MemoryPool *pool) {
    if (pool_ == pool) {
      return;
    }
    if (pool_) {
      pool_->buffered -= buffered_;
    }
    if (pool) {
      pool->buffered += buffered_;
    }
    pool_ = pool;
  }
  void charge(td::uint64 n) {
    buffered_ += n;
    if (pool_) {
      pool_->buffered += n;
    }
  }
  void release(td::uint64 n) {
    buffered_ -= n;
    if (pool_) {
      pool_->buffered -= n;
    }
  }
  td::uint64 buffered() const {
    return buffered_;
  }
  MemoryPool *pool() const {
    return pool_;
  }
  td::Status check(td::uint64 max_connection_buffered) const {
    if (buffered_ > max_connection_buffered) {
      return td::Status::Error(PSLICE() << "connection buffered limit exceeded: " << buffered_ << " > "
                                        << max_connection_buffered);
    }
    if (pool_ && pool_->max_bytes != 0 && pool_->buffered > pool_->max_bytes) {
      return td::Status::Error(PSLICE() << "pool buffered limit exceeded: " << pool_->buffered << " > "
                                        << pool_->max_bytes);
    }
    return td::Status::OK();
  }

 private:
  td::uint64 buffered_ = 0;
  MemoryPool *pool_ = nullptr;
};

// RAII membership in a byte pool. While it exists, the connection's reassembly bytes count toward the
// shared pool; if an eviction LRU is given (untrusted only), it also holds a node there. Constructing
// it joins the pool (and inserts the LRU node); destroying it removes both. The trusted pool passes
// lru == nullptr — trusted connections are pooled but never eviction-shed.
class FloodGuard::PoolMembership {
 public:
  PoolMembership(ConnectionMemory &memory, MemoryPool &pool, std::list<QuicConnectionId> *lru, QuicConnectionId cid)
      : memory_(&memory), lru_(lru), it_(lru ? lru->insert(lru->end(), cid) : std::list<QuicConnectionId>::iterator{}) {
    memory_->set_pool(&pool);
  }
  PoolMembership(PoolMembership &&other) noexcept
      : memory_(std::exchange(other.memory_, nullptr)), lru_(other.lru_), it_(other.it_) {
  }
  PoolMembership &operator=(PoolMembership &&) = delete;
  ~PoolMembership() {
    if (memory_) {
      memory_->set_pool(nullptr);
      if (lru_) {
        lru_->erase(it_);
      }
    }
  }
  // Record activity: move to the most-recently-active end (O(1), iterator stable). No-op on the LRU
  // without one (trusted pool). Stamps the activity time, so untrusted_lru_ stays time-ordered.
  void touch() {
    last_activity_ = td::Timestamp::now();
    if (lru_) {
      lru_->splice(lru_->end(), *lru_, it_);
    }
  }
  td::Timestamp last_activity() const {
    return last_activity_;
  }

 private:
  ConnectionMemory *memory_;
  std::list<QuicConnectionId> *lru_;
  std::list<QuicConnectionId>::iterator it_;
  td::Timestamp last_activity_ = td::Timestamp::now();
};

struct FloodGuard::StreamState : public td::HeapNode {
  QuicConnectionId cid;
  QuicStreamID sid;

  // charge_ counts exactly the bytes this stream holds in reassembly: it grows as data arrives and
  // is released the moment the buffer leaves reassembly — at extract() (dispatched downstream, where
  // the bytes move to inflight_charge_ until the stream closes) or mark_failed(). The dtor is only a
  // fallback for a stream torn down while still buffering.
  StreamState(QuicConnectionId cid, QuicStreamID sid, ConnectionMemory &memory) : cid(cid), sid(sid), charge_(memory) {
  }
  StreamState(StreamState &&) = delete;  // intrusive heap node: never move

  void append(td::BufferSlice data) {
    if (!data.empty()) {
      total_size_ += data.size();
      charge_.debit(data.size());
      builder_.append(std::move(data));
    }
  }

  // Buffering until the message is dispatched (extract) or the stream is failed; a terminated stream
  // has already released its reassembly charge and ignores further data.
  bool is_buffering() const {
    return !done_;
  }

  void mark_failed() {
    done_ = true;
    builder_ = {};      // free the reassembly buffer
    charge_.release();  // the bytes have left reassembly
  }

  td::Status check_limits() const {
    if (options_.max_size.has_value() && total_size_ > options_.max_size) {
      return td::Status::Error(PSLICE() << "stream size limit exceeded: max=" << *options_.max_size
                                        << " received=" << total_size_);
    }
    return td::Status::OK();
  }

  td::Status timeout_error() const {
    return td::Status::Error(PSLICE() << "stream timeout exceeded: received=" << total_size_);
  }

  td::uint64 total_size() const {
    return total_size_;
  }

  td::BufferSlice extract(MemoryPool *inflight) {
    done_ = true;  // message dispatched; no more data on this stream
    auto data = builder_.extract();
    charge_.release();  // the bytes leave reassembly...
    if (inflight) {
      inflight_charge_ = Charge<MemoryPool>(*inflight, data.size());  // ...and stay in flight until the stream closes
    }
    return data;
  }

  void set_options(StreamOptions options) {
    options_ = options;
  }

  // The app answered or reset this stream: its query is done, so the in-flight charge may die with
  // the stream.
  void mark_app_finished() {
    app_finished_ = true;
  }
  // Transport teardown of a stream whose query is still executing must not drop the charge — the
  // caller re-homes it until the app finishes.
  Charge<MemoryPool> take_unfinished_inflight() {
    return app_finished_ ? Charge<MemoryPool>{} : std::move(inflight_charge_);
  }

 private:
  td::BufferBuilder builder_;
  td::uint64 total_size_{0};
  StreamOptions options_;
  Charge<ConnectionMemory> charge_;
  Charge<MemoryPool> inflight_charge_;
  bool done_{false};
  bool app_finished_{false};
};

// Per-connection flood state the guard keeps its own record of, keyed by cid: the source IP, trust
// class and lifecycle flags, the half-open/shaper list iterators for O(1) removal, and the pooled
// reassembly/in-flight bookkeeping (memory, streams, pool membership, churn bucket).
struct FloodGuard::Conn {
  std::string ip;
  bool is_outbound = false;
  bool trusted = false;
  bool ready = false;
  bool is_handshaking = false;      // counted in half_open_by_ip_ / half_open_fifo_ (inbound only)
  td::uint64 max_message_size = 0;  // per-stream reassembly cap (the peer MTU)
  std::optional<std::list<QuicConnectionId>::iterator> half_open_it;
  std::optional<std::list<QuicConnectionId>::iterator> shaper_it;
  ConnectionMemory memory;  // declared before streams: their teardown releases into it
  std::map<QuicStreamID, StreamState> streams;
  // Set iff the connection is in a byte pool: owns its pool byte-contribution and (untrusted only)
  // its eviction-LRU node together. Declared after memory so it releases the pool while memory lives.
  std::optional<PoolMembership> membership;
  // Stream-churn bucket (replaced with the configured capacity/rate at on_handshake_done).
  adnl::RateLimiter churn{1, 1.0};
};

FloodGuard::FloodGuard(FloodLimits limits, Host &host)
    : limits_(limits)
    , host_(host)
    , conn_rate_(limits.conn_rate_capacity, limits.conn_rate_period)
    , global_conn_rate_(limits.global_conn_rate_capacity, limits.global_conn_rate_period)
    , untrusted_pool_{.max_bytes = limits.max_untrusted_buffered}
    , untrusted_inflight_{.max_bytes = limits.max_untrusted_inflight_bytes}
    , trusted_pool_{.max_bytes = limits.max_trusted_buffered}
    , trusted_inflight_{.max_bytes = limits.max_trusted_inflight_bytes} {
  aggregate_.total_rate = limits_.untrusted_ingress_rate;
}

FloodGuard::~FloodGuard() = default;

FloodGuard::Conn *FloodGuard::find_conn(QuicConnectionId cid) {
  auto it = conns_.find(cid);
  return it == conns_.end() ? nullptr : it->second.get();
}

bool FloodGuard::accepting_handshakes() const {
  // Peek only (the src IP is unproven here, so nothing is spent); a Retry is amplification-safe,
  // so absent global saturation we always answer with one.
  return global_conn_rate_.ready_at().is_in_past();
}

td::Status FloodGuard::admit(const std::string &ip) {
  // Discouragement is independent of max_conns_per_ip: it must survive `--quic-flood-control -1`.
  // The throttle slot is consumed at connection creation (on_inbound_created), not here: the
  // stateless-retry flow revisits this check with the token, and consuming per packet would lock
  // the retried Initial out forever.
  if (auto it = discouraged_.find(ip); it != discouraged_.end()) {
    if (it->second.until.is_in_past()) {
      discouraged_.erase(it);
    } else if (!it->second.next_allowed.is_in_past()) {
      return td::Status::Error("address is discouraged");
    }
  }
  if (limits_.max_conns_per_ip.has_value()) {
    if (auto it = live_by_ip_.find(ip); it != live_by_ip_.end() && it->second >= *limits_.max_conns_per_ip) {
      return td::Status::Error("flood control overflow");
    }
    TRY_STATUS(conn_rate_.take_new_connection(ip));
    if (!global_conn_rate_.take()) {
      return td::Status::Error("global new connection rate limit exceeded");
    }
  }
  // Per-IP half-open cap: silently drop the datagram (stateless refusal is amplification-limited).
  if (limits_.max_handshaking_conns_per_ip != 0) {
    if (auto it = half_open_by_ip_.find(ip);
        it != half_open_by_ip_.end() && it->second >= limits_.max_handshaking_conns_per_ip) {
      return td::Status::Error("too many handshaking connections from address");
    }
  }
  return td::Status::OK();
}

void FloodGuard::on_inbound_created(QuicConnectionId cid, const std::string &ip) {
  auto conn = std::make_unique<Conn>(Conn{.ip = ip, .is_outbound = false});
  auto &c = *conn;
  conns_.emplace(cid, std::move(conn));

  c.is_handshaking = true;
  half_open_by_ip_[ip]++;
  c.half_open_it = half_open_fifo_.insert(half_open_fifo_.end(), cid);
  // Consume the throttle slot for a discouraged IP here (at admission), not per packet: this is the
  // one connection allowed this throttle period; the next is refused until it elapses.
  if (auto it = discouraged_.find(ip); it != discouraged_.end()) {
    it->second.next_allowed = td::Timestamp::in(limits_.discourage_throttle_period);
  }
  if (limits_.max_conns_per_ip.has_value()) {
    live_by_ip_[ip]++;
  }
  // Inbound connections start shaped from creation (covers the pre-handshake window); a trust
  // upgrade unshapes them in on_handshake_done. No-op when untrusted_ingress_rate == 0.
  apply_caps(cid, c);
  // Global half-open cap: evict the oldest (front), never the just-admitted newcomer (at the back).
  // 503 carries only the short evict-discourage throttle (evict_discourage_period, a few seconds),
  // not the long churn/forbidden ban, so an evicted honest peer can retry almost at once.
  while (limits_.max_handshaking_conns != 0 && half_open_fifo_.size() > limits_.max_handshaking_conns) {
    close_connection(half_open_fifo_.front(), CloseCode::service_unavailable);
  }
}

void FloodGuard::on_outbound_created(QuicConnectionId cid) {
  // Our own dial: never pooled, shaped or discouraged. The egress cap stays log-only even once the
  // peer proves trusted, so a legitimate large answer we requested is never reset.
  auto conn = std::make_unique<Conn>(Conn{.is_outbound = true});
  auto &c = *conn;
  conns_.emplace(cid, std::move(conn));
  apply_caps(cid, c);
}

td::Status FloodGuard::on_handshake_done(QuicConnectionId cid, bool trusted, td::uint64 max_message_size) {
  auto *conn = find_conn(cid);
  if (!conn) {
    return td::Status::OK();
  }
  leave_half_open(*conn);  // no longer half-open
  conn->trusted = trusted;
  conn->max_message_size = max_message_size;
  conn->churn = adnl::RateLimiter(std::max<td::uint32>(1, limits_.churn_capacity),
                                  limits_.churn_rate > 0.0 ? 1.0 / limits_.churn_rate : 1.0);
  refresh_pool_membership(cid, *conn);  // route into its trust-class pool (untrusted with LRU)
  if (reserved_only_ && is_untrusted_inbound(*conn)) {
    // Reserved-only mode: a new untrusted inbound connection is refused. The caller routes this
    // through the failed-handshake teardown (a non-discouraging ok-close) without announcing it;
    // on_connection_closed then releases the pool membership just joined. The connection stays NOT
    // ready, so on_stream_data refuses any 1-RTT data coalesced ahead of the (deferred) teardown:
    // otherwise a completed message would take an in-flight charge that nothing ever releases (the
    // app was never told about this connection, so it never finishes the stream) — a permanent leak.
    return td::Status::Error("reserved-only mode: rejecting untrusted peer");
  }
  conn->ready = true;  // classified and admitted: accept stream data and dispatch messages
  apply_caps(cid, *conn);
  return td::Status::OK();
}

void FloodGuard::on_peer_info_updated(QuicConnectionId cid, bool trusted, td::uint64 max_message_size) {
  auto *conn = find_conn(cid);
  if (!conn) {
    return;
  }
  conn->trusted = trusted;
  conn->max_message_size = max_message_size;
  refresh_pool_membership(cid, *conn);  // moves bytes and LRU membership in/out of the untrusted pool
  // A live inbound connection's trust flipped: unshape it on upgrade (dump debt), re-shape on
  // downgrade so an untrusted peer can't keep line-rate ingress. Re-caps in either direction.
  apply_caps(cid, *conn);
}

void FloodGuard::close_connection(QuicConnectionId cid, CloseCode code) {
  auto *conn = find_conn(cid);
  if (!conn) {
    return;
  }
  // Discourage the peer's IP for a duration graduated by close reason; never a trusted or outbound
  // peer (a validator's normal reconnect must not suffer). A stronger verdict already in effect is
  // never shortened by a weaker one. Read the record before drop_connection re-enters teardown.
  if (double period = discourage_period_for(limits_, code); period > 0 && !conn->trusted && !conn->is_outbound) {
    auto &d = discouraged_[conn->ip];
    d.until = std::max(d.until, td::Timestamp::in(period));
    d.next_allowed = td::Timestamp::in(limits_.discourage_throttle_period);
  }
  // Leave the pool and eviction LRU immediately, before drop_connection re-enters teardown, so an
  // eviction sweep in flight sees this connection counted at most once (no dedupe set needed).
  conn->membership.reset();
  host_.drop_connection(cid, code);
}

void FloodGuard::on_connection_closed(QuicConnectionId cid) {
  auto it = conns_.find(cid);
  if (it == conns_.end()) {
    return;  // idempotent for unknown cid (re-entrant teardown, stale drains)
  }
  auto &conn = *it->second;
  leave_half_open(conn);  // no-op once the handshake completed
  dequeue_shaper(conn);
  // A still-running query's in-flight charge outlives the stream: re-home it until on_stream_finished.
  for (auto &[sid, state] : conn.streams) {
    if (state.in_heap()) {
      stream_deadlines_.erase(&state);
    }
    orphan_unfinished_inflight(cid, sid, state);
  }
  if (!conn.is_outbound && limits_.max_conns_per_ip.has_value()) {
    if (auto lit = live_by_ip_.find(conn.ip); lit != live_by_ip_.end() && --lit->second == 0) {
      live_by_ip_.erase(lit);
    }
  }
  conns_.erase(it);  // ~Conn: PoolMembership leaves the pool and LRU, streams release their charges
}

void FloodGuard::set_reserved_only(bool reserved_only) {
  reserved_only_ = reserved_only;
  if (!reserved_only_) {
    return;
  }
  for (auto &[cid, conn] : conns_) {
    if (is_untrusted_inbound(*conn)) {  // shed untrusted (policy, not abuse); keep trusted
      pending_closes_.push_back({cid, CloseCode::forbidden});
    }
  }
}

td::Status FloodGuard::on_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data, bool fin) {
  // Data before successful classification is refused: the guard tracks the connection from creation,
  // but its trust class (and per-stream cap) is only known once on_handshake_done has run.
  if (auto *conn = find_conn(cid); !conn || !conn->ready) {
    return td::Status::Error("unknown connection");
  }
  TRY_RESULT(stream, get_or_create_stream(cid, sid));
  auto &state = *stream.state;
  if (stream.inserted) {
    // Untrusted peers get the tight deadline; trusted peers and our own outbound streams get the
    // generous one (still bounded, so a stuck stream can't pin reassembly forever).
    double timeout = is_untrusted_inbound(*stream.conn) ? limits_.stream_timeout : limits_.trusted_stream_timeout;
    apply_stream_options(
        state, StreamOptions{.max_size = stream.conn->max_message_size, .deadline = td::Timestamp::in(timeout)});
  }
  if (!state.is_buffering()) {
    LOG(INFO) << "got data for finished stream, ignore cid=" << cid << " sid=" << sid;
    return td::Status::Error("stream finished");
  }
  if (stream.conn->membership) {
    stream.conn->membership->touch();
  }
  state.append(std::move(data));
  if (is_trusted_inbound(*stream.conn)) {
    maybe_warn_trusted_buffered();
  }
  auto status = state.check_limits();
  if (status.is_ok()) {
    status = stream.conn->memory.check(limits_.max_connection_buffered);
  }
  if (status.is_ok() && !fin) {
    return td::Status::OK();
  }
  // Dispatch admission: from here the message runs to completion downstream and closing the
  // connection can't reclaim it, so the bytes move from the reassembly accounting to the hard-capped
  // in-flight pool for the peer's trust class. The stream holds the charge for the life of the query
  // it carries (orphaned, not dropped, if the peer tears the stream down early). Our own outbound
  // streams are uncharged (bounded by max_answer_size instead).
  auto *inflight = inflight_pool_for(*stream.conn);
  if (status.is_ok() && inflight && inflight->would_exceed(state.total_size())) {
    status = td::Status::Error("in-flight budget exhausted");
  }
  if (status.is_error()) {
    // A trusted peer hitting a limit is a compromise/misconfiguration signal, not routine abuse.
    LOG_IF(WARNING, is_trusted_inbound(*stream.conn))
        << "trusted stream failed (possible compromise) cid=" << cid << " sid=" << sid << " due to " << status.error();
    LOG_IF(INFO, !is_trusted_inbound(*stream.conn))
        << "close stream cid=" << cid << " sid=" << sid << " due to " << status.error();
    fail_stream(state, status.clone());  // deliver the failure first; the returned status resets the stream
    return status;
  }
  host_.deliver(cid, sid, state.extract(inflight));
  return td::Status::OK();
}

void FloodGuard::on_local_stream(QuicConnectionId cid, QuicStreamID sid, StreamOptions options) {
  auto R = get_or_create_stream(cid, sid);
  if (R.is_error()) {
    return;
  }
  if (!options.deadline) {
    // Self-initiated streams get the generous bound by default, so a peer that never acks or answers
    // cannot pin our stream (and our max_streams credit toward it) forever.
    options.deadline = td::Timestamp::in(limits_.trusted_stream_timeout);
  }
  apply_stream_options(*R.ok().state, options);
}

void FloodGuard::on_stream_closed(QuicConnectionId cid, QuicStreamID sid) {
  auto *conn = find_conn(cid);
  if (!conn) {
    return;
  }
  auto sid_it = conn->streams.find(sid);
  if (sid_it == conn->streams.end()) {
    return;
  }
  // Churn = a peer walking away from a stream it opened, mid-message: still buffering (not dispatched,
  // not failed by our own deadline/limit policy — those clear is_buffering first).
  bool peer_abandoned = sid_it->second.is_buffering() && is_peer_initiated(conn->is_outbound, sid);
  // A trusted peer churning is a compromise signal — log it, but never discourage or close it.
  LOG_IF(WARNING, peer_abandoned && is_trusted_inbound(*conn))
      << "trusted peer stream churn (possible compromise) cid=" << cid << " sid=" << sid;
  bool abandoned = peer_abandoned && is_untrusted_inbound(*conn);  // only untrusted trips the churn bucket
  if (sid_it->second.in_heap()) {
    stream_deadlines_.erase(&sid_it->second);
  }
  orphan_unfinished_inflight(cid, sid, sid_it->second);
  conn->streams.erase(sid_it);
  if (abandoned && !conn->churn.take()) {
    LOG(INFO) << "close connection cid=" << cid << " due to stream churn";
    pending_closes_.push_back({cid, CloseCode::too_many_requests});
  }
}

void FloodGuard::on_stream_finished(QuicConnectionId cid, QuicStreamID sid) {
  if (orphaned_inflight_.erase({cid, sid})) {
    return;
  }
  if (auto *conn = find_conn(cid)) {
    if (conn->membership) {
      conn->membership->touch();  // finishing a query is activity — keep it off the idle sweep
    }
    if (auto s = conn->streams.find(sid); s != conn->streams.end()) {
      s->second.mark_app_finished();
    }
  }
}

void FloodGuard::note_activity(QuicConnectionId cid) {
  if (auto *conn = find_conn(cid); conn && conn->membership) {
    conn->membership->touch();
  }
}

void FloodGuard::on_ingress_debt(QuicConnectionId cid, td::uint64 debt) {
  auto *conn = find_conn(cid);
  if (!conn) {
    return;
  }
  // Enqueue a connection that accrued shaped debt; the paced drain grants its credit. Granting only
  // there (never inline) keeps every grant subject to the fair-share cap, so no one connection can
  // drain the bucket on its own packet. The pre-shaping burst covers the zero-contention case.
  if (debt != 0) {
    if (!conn->shaper_it) {
      conn->shaper_it = shaper_waiters_.insert(shaper_waiters_.end(), cid);
      if (!next_shaper_drain_) {
        next_shaper_drain_ = td::Timestamp::in(IngressAggregate::DRAIN_TICK);
      }
    }
  } else {
    dequeue_shaper(*conn);
  }
}

void FloodGuard::tick(td::Timestamp now) {
  // Stream-deadline sweep: fail each still-buffering pending stream (deliver the error, then reset —
  // the timer path owns this stream, so the reset is direct).
  while (!stream_deadlines_.empty() && td::Timestamp::at(stream_deadlines_.top_key()).is_in_past(now)) {
    auto *state = static_cast<StreamState *>(stream_deadlines_.pop());
    if (state->is_buffering()) {
      fail_stream(*state, state->timeout_error());
      host_.reset_stream(state->cid, state->sid);
    }
  }
  for (auto &c : std::exchange(pending_closes_, {})) {  // churn / reserved sheds since the last tick
    close_connection(c.first, c.second);
  }
  evict_over_byte_budget();
  evict_over_count_budget();
  close_inactive_untrusted(now);
  conn_rate_.cleanup();
  td::table_remove_if(discouraged_, [](const auto &it) { return it.second.until.is_in_past(); });
}

void FloodGuard::drain_shaper(td::Timestamp now) {
  if (!(next_shaper_drain_ && next_shaper_drain_.is_in_past())) {
    return;  // internally paced: a no-op before the pace tick
  }
  // One paced pass over the waiters: refill once, then serve round-robin, capping each grant at its
  // fair share of the remaining bucket (max-min: a waiter that wants less than its share leaves the
  // surplus for the rest, so a big debtor still gets it when others are small). A served-and-cleared
  // connection leaves the queue, a partly-served one rotates to the back. O(served), one reschedule.
  aggregate_.refill(now);
  size_t n = shaper_waiters_.size();
  for (size_t i = 0; i < n && aggregate_.tokens > 0; i++) {
    auto share = static_cast<td::uint64>(aggregate_.tokens) / (n - i);
    auto cap = std::max<td::uint64>(share, IngressAggregate::MIN_GRANT);  // keep grants chunky
    auto cid = shaper_waiters_.front();
    auto *conn = find_conn(cid);
    if (!conn) {
      shaper_waiters_.pop_front();
      continue;
    }
    auto grant = host_.grant_ingress(cid, now, cap);
    if (grant.debt == 0) {
      dequeue_shaper(*conn);  // fully served (or gone == {0, 0})
    } else if (grant.granted) {
      shaper_waiters_.splice(shaper_waiters_.end(), shaper_waiters_, *conn->shaper_it);  // rotate
    } else {
      break;  // front waiter is below the floor and got nothing: everyone else waits too
    }
  }
  next_shaper_drain_ =
      shaper_waiters_.empty() ? td::Timestamp::never() : td::Timestamp::in(IngressAggregate::DRAIN_TICK, now);
}

td::Timestamp FloodGuard::next_wakeup() const {
  td::Timestamp ts = td::Timestamp::never();
  ts.relax(conn_rate_.next_cleanup_at());
  ts.relax(next_shaper_drain_);
  if (!stream_deadlines_.empty()) {
    ts.relax(td::Timestamp::at(stream_deadlines_.top_key()));
  }
  if (limits_.untrusted_inactivity_timeout > 0 && !untrusted_lru_.empty()) {
    // Wake to run the idle sweep even when no stream deadline is pending.
    ts.relax(inactivity_deadline(conns_.at(untrusted_lru_.front())->membership->last_activity()));
  }
  if (!pending_closes_.empty()) {
    ts.relax(td::Timestamp::now());  // drain queued churn / reserved sheds on the next tick
  }
  return ts;
}

void FloodGuard::leave_half_open(Conn &conn) {
  if (!conn.is_handshaking) {
    return;
  }
  conn.is_handshaking = false;
  if (conn.half_open_it) {
    half_open_fifo_.erase(*conn.half_open_it);
    conn.half_open_it.reset();
  }
  if (auto it = half_open_by_ip_.find(conn.ip); it != half_open_by_ip_.end() && --it->second == 0) {
    half_open_by_ip_.erase(it);
  }
}

void FloodGuard::dequeue_shaper(Conn &conn) {
  if (conn.shaper_it) {
    shaper_waiters_.erase(*conn.shaper_it);
    conn.shaper_it.reset();
  }
}

FloodGuard::TransportCaps FloodGuard::compute_caps(const Conn &conn) const {
  // Outbound (our own dials) always log-only, never shaped, even when trusted. Inbound is enforced;
  // trusted gets the larger egress ceiling and is unshaped, untrusted the per-connection budget and
  // shaping.
  if (conn.is_outbound) {
    return {limits_.max_connection_buffered, false, false};
  }
  return {conn.trusted ? limits_.max_trusted_connection_egress : limits_.max_connection_buffered, true, !conn.trusted};
}

void FloodGuard::apply_caps(QuicConnectionId cid, Conn &conn) {
  auto caps = compute_caps(conn);
  if (!caps.shape_ingress) {
    dequeue_shaper(conn);  // leaving the shaper: drop any pending drain-queue slot
  }
  host_.configure(cid, caps);
}

bool FloodGuard::is_untrusted_inbound(const Conn &conn) {
  return !conn.is_outbound && !conn.trusted;
}
bool FloodGuard::is_trusted_inbound(const Conn &conn) {
  return !conn.is_outbound && conn.trusted;
}

// The in-flight pool a dispatched message is charged to (nullptr = uncharged: our own outbound,
// bounded instead by max_answer_size).
FloodGuard::MemoryPool *FloodGuard::inflight_pool_for(const Conn &conn) {
  return is_untrusted_inbound(conn) ? &untrusted_inflight_ : is_trusted_inbound(conn) ? &trusted_inflight_ : nullptr;
}

// The reassembly pool a connection belongs to (nullptr = unpooled: our own outbound).
FloodGuard::MemoryPool *FloodGuard::buffered_pool_for(const Conn &conn) {
  return is_untrusted_inbound(conn) ? &untrusted_pool_ : is_trusted_inbound(conn) ? &trusted_pool_ : nullptr;
}

// Log once when the trusted reassembly pool crosses the soft watermark (a validator sending enough to
// approach the hard cap is a compromise/misconfiguration signal), reset when it drops.
void FloodGuard::maybe_warn_trusted_buffered() {
  bool over = trusted_pool_.max_bytes != 0 && trusted_pool_.buffered > trusted_pool_.max_bytes / 4 * 3;
  if (over && !trusted_pool_warned_) {
    LOG(WARNING) << "trusted reassembly pool over soft watermark: " << trusted_pool_.buffered << " / "
                 << trusted_pool_.max_bytes;
  }
  trusted_pool_warned_ = over;
}

// Route the connection into its trust-class pool (untrusted with LRU, trusted without). Idempotent, so
// it is safe to call on classification and on every trust flip; joining/leaving/moving the bytes is
// keyed on the current pool, since both classes hold a membership.
void FloodGuard::refresh_pool_membership(QuicConnectionId cid, Conn &conn) {
  auto *want = buffered_pool_for(conn);
  if (conn.memory.pool() == want) {
    return;
  }
  conn.membership.reset();  // leave the old pool (moves bytes out)
  if (want) {
    conn.membership.emplace(conn.memory, *want, want == &untrusted_pool_ ? &untrusted_lru_ : nullptr, cid);
  }
}

td::Result<FloodGuard::StreamLookup> FloodGuard::get_or_create_stream(QuicConnectionId cid, QuicStreamID sid) {
  auto it = conns_.find(cid);
  if (it == conns_.end()) {
    return td::Status::Error("unknown connection");
  }
  auto &conn = *it->second;
  auto it2 = conn.streams.try_emplace(sid, cid, sid, conn.memory);
  return StreamLookup{.state = &it2.first->second, .inserted = it2.second, .conn = &conn};
}

// The transport tore the stream down while its query still runs: the charge outlives the stream and
// is released by on_stream_finished when the app finishes the query (guaranteed — every dispatched
// query ends in send_stream-with-fin or shutdown_stream).
void FloodGuard::orphan_unfinished_inflight(QuicConnectionId cid, QuicStreamID sid, StreamState &state) {
  if (auto charge = state.take_unfinished_inflight(); charge.amount() != 0) {
    orphaned_inflight_.emplace(std::pair{cid, sid}, std::move(charge));
  }
}

void FloodGuard::apply_stream_options(StreamState &state, const StreamOptions &options) {
  state.set_options(options);
  if (options.deadline) {
    if (state.in_heap()) {
      stream_deadlines_.fix(options.deadline.at(), &state);
    } else {
      stream_deadlines_.insert(options.deadline.at(), &state);
    }
  } else if (state.in_heap()) {
    stream_deadlines_.erase(&state);
  }
}

void FloodGuard::fail_stream(StreamState &state, td::Status error) {
  if (state.in_heap()) {
    stream_deadlines_.erase(&state);
  }
  state.mark_failed();  // stops buffering and releases the reassembly charge
  host_.deliver(state.cid, state.sid, std::move(error));
}

td::Timestamp FloodGuard::inactivity_deadline(td::Timestamp last_activity) const {
  return td::Timestamp::in(limits_.untrusted_inactivity_timeout, last_activity);
}

// Byte budget: once past the high watermark, shed the fattest pooled connections until the pool is
// back under its low watermark. Capacity shedding, not a misbehavior verdict. close_connection leaves
// the pool and LRU immediately, so a connection is counted at most once — no dedupe set needed.
void FloodGuard::evict_over_byte_budget() {
  auto reclaim = untrusted_pool_.bytes_to_reclaim();
  if (reclaim == 0) {
    return;
  }
  std::vector<std::pair<td::uint64, QuicConnectionId>> victims;
  for (auto cid : untrusted_lru_) {
    if (auto bytes = conns_.at(cid)->memory.buffered(); bytes > 0) {
      victims.emplace_back(bytes, cid);
    }
  }
  std::sort(victims.begin(), victims.end(), std::greater<>());  // fattest first
  for (auto &[bytes, cid] : victims) {
    if (reclaim == 0) {
      break;
    }
    LOG(INFO) << "evict connection cid=" << cid << " holding " << bytes << " bytes from the untrusted pool";
    close_connection(cid, CloseCode::service_unavailable);
    reclaim -= std::min(reclaim, bytes);
  }
}

// Count budget: shed the least-recently-active pooled connections over the cap. The LRU is ordered, so
// this walks from the front (oldest) — no scan or sort.
void FloodGuard::evict_over_count_budget() {
  if (limits_.max_untrusted_connections == 0) {
    return;  // unlimited
  }
  while (untrusted_lru_.size() > limits_.max_untrusted_connections) {
    auto cid = untrusted_lru_.front();
    LOG(INFO) << "evict connection cid=" << cid << " (untrusted connection cap reached)";
    close_connection(cid, CloseCode::service_unavailable);
  }
}

// Close untrusted connections idle past the inactivity window. untrusted_lru_ is ordered by last
// activity, so the front has the earliest deadline — stop at the first that hasn't elapsed.
void FloodGuard::close_inactive_untrusted(td::Timestamp now) {
  if (limits_.untrusted_inactivity_timeout <= 0) {
    return;
  }
  while (!untrusted_lru_.empty()) {
    auto cid = untrusted_lru_.front();
    auto &conn = *conns_.at(cid);
    if (!inactivity_deadline(conn.membership->last_activity()).is_in_past(now)) {
      break;  // the rest are more recently active
    }
    LOG(INFO) << "close idle untrusted connection cid=" << cid;
    close_connection(cid, CloseCode::ok);  // ok => no discourage; inactivity is not abuse
  }
}

}  // namespace ton::quic
