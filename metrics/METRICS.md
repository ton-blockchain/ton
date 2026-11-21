# TON node metrics

Every Prometheus metric a `validator-engine` exposes.

## Enabling the endpoint

Command-line only — there is no config-file field:

```
validator-engine --exporter-address <host:port>
```

Scrape `GET /metrics`. Any other path returns 404, any other method 405. There is no
authentication and no TLS, so bind it to a private interface.

The response is `application/openmetrics-text; version=1.0.0; charset=utf-8`, chunked, terminated
by `# EOF`. Each family carries a `# TYPE` line. `# HELP` is never emitted.

## How names are built

The exporter seeds a root name segment (`ton` by default, `PrometheusExporter::create()`), and
each nesting level appends a segment joined with `_`:

- `Context::with_name(seg)` and `Context::collect(node, "seg")` push a segment; `collect(node)`
  with no name adds none, letting the inner node supply the final segment.
- `Counter` renders as `<segments>_total`.
- `Gauge<T>` renders as `<segments>`, except `std::chrono` types which append `_seconds`.
- `Labeled<Inner, L...>` adds one label per axis over a **closed** value set — `direction`, `kind`,
  `reason`, `result`, `state`, `trust`, `workchain`, `source`. (An `outcome` axis is defined too, but
  its only holder `TransferStats` is never instantiated, so no family carries it.) **Every cell of a
  closed axis is emitted on every scrape**, including zero-valued ones, so all such label combinations below are
  always present in the exposition (which is why the permanently-zero series in Known gaps still
  show up).
  The **open** label axes behave differently and emit only values actually observed: `code` on
  the HTTP responses family, `tl` on the traffic and latency buckets, `op` on the perf families, and
  `type` / `scheduler` on the actor families. The `tl` buckets always
  emit their `tl="unknown"` cell, populated or not, and a latency bucket always emits both of its
  families even when nothing was ever observed.

Registered collectors, in order: the exporter itself, `AdnlNetworkManager`, `Adnl`, `QuicSender`,
`Rldp` (rldp2), `Overlays`, `ValidatorManagerInterface`.

## Scrape semantics

Collection is asynchronous and **sequential**: `gather()` awaits `td::actor::ask(...)` on each
collector one at a time, so a scrape costs one round-trip per collector and the subsystems are
sampled at slightly different instants. The exposition is therefore not a consistent point-in-time
snapshot across subsystems.

Scrapes are **coalesced**: a `GET /metrics` that arrives while a gather is already running does not
start a second one — it waits and is served the same rendered body. Concurrent or retrying scrapers
therefore see identical output and cost one gather between them. Each request still counts in
`ton_exporter_collections_total`.

A gather that **fails** — any collector returning an error — answers every waiting scraper with
HTTP 500 and an empty body. Either way the response and its payload are built, filled and completed
before the connection actor is handed them, so the payload is never shared with the HTTP writer
while it is still mutable and the two actors have nothing to race over. On that path
`ton_exporter_last_collection_duration_seconds` is not updated, while
`ton_exporter_last_collection_timestamp_seconds` was already advanced before the gather started: a
node whose gather fails on every scrape keeps a perfectly fresh timestamp, so only the `up == 0`
alert arm catches it (see *Is the exporter itself healthy?*).

Values are cumulative snapshots; a scrape never resets them. Three subsystems (ADNL peer pairs,
RLDP2 connections, overlays) accumulate counters on their own actor threads and merge deltas into a
process-wide aggregate during the scrape — see the notes in those sections.

---

## Exporter

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_exporter_collectors` | gauge | — | Registered collector callbacks: 7 in a full validator-engine, 6 when no DHT node is configured (which also drops the `ton_overlay_*` families, since the overlay manager is only created with one). |
| `ton_exporter_collections_total` | counter | — | Scrapes accepted, including ones coalesced into a gather already in flight. |
| `ton_exporter_last_collection_duration_seconds` | gauge | — | Duration of the **previous** scrape (it is set after the current one is already serialized). |
| `ton_exporter_last_collection_timestamp_seconds` | gauge | — | Unix time at which the current scrape started. |
| `ton_perf_ops_total` | counter | `op` | Executions of a `TD_PERF_COUNTER` site, read straight from the process-global registry on each scrape (its totals are already cumulative, so nothing is mirrored). `op` is the site name (`Ed25519_sign`, `Ed25519_verify_signature`, `cell_load`, `cell_store`, `raptor_solve`, …); a site registers on first execution, so one that has never run emits no series. |
| `ton_perf_op_ticks_total` | counter | `op` | Raw `rdtsc` ticks elapsed between each instrumented operation's entry and exit. Blocking and descheduling are included, so this is not OS CPU time. Absolute values are machine-specific; divide by `ton_actor_ticks_per_second` per target to obtain elapsed seconds before deriving rates or averages. |

## HTTP server

Only the exporter's own server is registered, hence the constant `server="exporter"` label.

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_http_server_connections_active` | gauge | `server` | Currently open TCP connections. |
| `ton_http_server_connections_total` | counter | `server` | Accepted TCP connections. |
| `ton_http_server_requests_total` | counter | `server` | HTTP requests received, any path or method. |
| `ton_http_server_responses_total` | counter | `server`, `code` | Responses by status code. For this server: `200`, `404`, `405`, `500` when the gather behind a `/metrics` scrape failed, and `-1` when the response promise failed. |

## Actors

The actor framework's own view of the scheduler group hosting the exporter, read at scrape time from
tdactor's per-actor-class stat tables (`ActorTypeStatImpl`) and scheduler state. A group registry
creates one table lazily for each executing thread and worker kind, then retains it until the group
is destroyed. This keeps independent groups isolated in tools such as `bench-rldp --both` and
preserves totals after a worker exits. Nothing is mirrored into the exporter: the collector only
reads, aggregates and demangles the counters tdactor already keeps. Every binary that runs the
exporter gets this tier, not just `validator-engine`.

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_actor_busy_ticks_total` | counter | `type` | Raw `rdtsc` ticks attributed to completed message handlers of this actor class or completed resumptions of the synthetic coroutine type. Signal-only work, gaps between messages and executor finish bookkeeping are not counted. Blocking and descheduling inside a handler are included. This is per-type attribution, not total worker occupancy. Divide by `ton_actor_ticks_per_second` per target before aggregating. |
| `ton_actor_messages_total` | counter | `type` | Messages executed. |
| `ton_actor_executions_total` | counter | `type` | Executor batches: one worker's uninterrupted run over an actor's signals and mailbox. `rate(messages) / rate(executions)` is the class's batching ratio — how many messages an average wakeup amortizes its scheduling cost over. A batch can drain **zero** messages (an alarm signal whose timestamp is not due yet, queue-bookkeeping signals), so the ratio reads below 1 for signal-churny classes; a class that genuinely batches reads well above it. |
| `ton_actor_created_total` | counter | `type` | Actors of this class ever created. |
| `ton_actor_alive` | gauge | `type` | Actors of this class currently alive (created − destroyed). |
| `ton_actor_max_message_ticks` | gauge | `type`, `window` | Raw ticks for the longest single message execution seen in the approximate window. |
| `ton_actor_max_execute_ticks` | gauge | `type`, `window` | Raw ticks for the longest timed executor drain in the approximate window: one worker processing an actor's signals and mailbox before finish-time bookkeeping. Actor teardown runs during `finish()` and is timed as a message after this timer stops, so `max_message_ticks` can exceed this value. |
| `ton_actor_max_batch_messages` | gauge | `type`, `window` | Most messages drained in one executor batch in the window — the peak of the batching ratio above, and the head-of-line blocking companion to `max_execute_ticks` (a long batch is either a few slow messages or many fast ones; this tells which). |
| `ton_actor_max_queue_ticks` | gauge | `type`, `window` | Raw ticks for the longest wait between an actor becoming runnable and a worker starting it. This is scheduling latency, not execution. A sample is discarded if the raw counter regresses across workers, rather than wrapping the unsigned duration. |
| `ton_actor_worker_busy_ticks_total` | counter | `worker=io\|cpu` | Raw ticks in outer dispatch scopes, split by worker kind. The scope covers the entire actor or coroutine dispatch, including signal-only work, gaps between message handlers and finish bookkeeping. A scrape includes elapsed time in the current scope, so the counter remains live during a long dispatch. It excludes idle poll/wait time. `io` is the scheduler's poll thread and `cpu` its worker pool. |
| `ton_actor_worker_messages_total` | counter | `worker=io\|cpu` | Completed messages and coroutine resumptions, split by the worker that ran them. |
| `ton_actor_worker_threads` | gauge | `worker=io\|cpu` | How many threads of each kind exist: one `io` per scheduler and `cpu_threads_count` `cpu` per scheduler, summed over the group. The denominator for the per-kind utilisation recipe below. |
| `ton_actor_scheduler_threads` | gauge | `scheduler` | Threads the scheduler owns: its cpu workers plus its one io worker. |
| `ton_actor_scheduler_local_queue_length` | gauge | `scheduler` | Runnable entries — actors with mail, and resumable coroutines — sitting in the scheduler's per-cpu-worker work-stealing queues, summed. See *Known gaps* for the two queues this does not see. |
| `ton_actor_scheduler_workers_active` | gauge | `scheduler` | Workers (io + cpu) that were inside an actor or coroutine dispatch at the instant of the scrape. |
| `ton_actor_scheduler_current_execute_seconds` | gauge | `scheduler` | Of those, how long the longest-running dispatch had already been executing, in seconds; `0` when none is active. A single point sample, but the only live view of a wedged worker. |
| `ton_actor_stats_enabled` | gauge | — | 1 if `td::actor::set_debug(true)` has run, 0 otherwise. |
| `ton_actor_ticks_per_second` | gauge | — | This target's calibrated `rdtsc` frequency. Divide tick counters/gauges by it in PromQL before aggregating targets. |

**The per-type tier is gated.** Unless `td::actor::set_debug(true)` ran, tdactor hands out a null
stat reference on every execution and no class is registered, so the nine labelled families are
still emitted but have **no samples at all**. The `worker` message counter combines actor messages
from the group registry with resumptions recorded by each worker's `Debug`; its busy counter comes
from those same workers' outer dispatch scopes. Both use the same gate and keep
emitting their closed `io`/`cpu` cells at zero. `validator-engine` calls `set_debug(true)`
unconditionally at startup, so on a node this tier is always on. `ton_actor_stats_enabled` is how a
dashboard tells "off" from "idle" — an
empty per-type tier with `ton_actor_stats_enabled == 1` really does mean nothing ran. The gate is
consulted per execution rather than per actor, so it applies process-wide the moment it is set. The
per-scheduler families do not depend on it, except `workers_active` /
`current_execute_seconds`, which read `core::Debug` and are gated the same way.

**TSC-derived values stay as raw ticks.** Rescaling a cumulative counter with a newly estimated
frequency on every scrape can make it decrease, which Prometheus interprets as a reset. The exporter
therefore emits raw ticks plus `ton_actor_ticks_per_second`, calibrated as elapsed ticks / monotonic
wall time since exporter construction (with the platform estimate during its first 0.1 s). Divide
each target before `sum`/`max`, because frequencies may differ:

```promql
sum by (type) (
  rate(ton_actor_busy_ticks_total[5m])
  / on (job, instance) group_left
    max by (job, instance) (ton_actor_ticks_per_second)
)
```

`ton_actor_scheduler_current_execute_seconds` is different: it comes directly from the monotonic
clock and is already seconds.

**The `type` axis is the demangled C++ class of the actor** (`ton::validator::ValidatorManagerImpl`,
`td::actor::ActorStats`, …), taken from `typeid` of the running object, so a subclass reports as
itself. It is **open** but bounded by the binary, not by anything a peer controls. A class appears
only after one of its actors starts its first execution — the table is populated at runtime, not on
link — so the installed binary and the workload determine cardinality, and a small utility stays
small.

**Windowed maxima, not instantaneous gauges** — hence the explicit `window="recent"` label. The
underlying counter keeps two nominal 600-second TSC buckets and reports the max over both. Exact wall
duration is platform-dependent where the TSC frequency is approximate; no wall-clock call is added
to the actor hot path. A recent maximum falls to zero after two full buckets without an update. The
counter cannot miss a spike between scrapes; longer horizons are recoverable with `max_over_time`.

**Scheduler-group scoped, but not split by individual scheduler.** The per-type families walk the
table registry in the exporter actor's scheduler group; the `worker` families combine those tables
with each worker's `Debug`. Their scope therefore matches `ton_actor_worker_threads`. Within that
group, actor classes are still merged across its schedulers. Only the four
`ton_actor_scheduler_*` families carry a `scheduler` label; there is no `type` × `worker` or
`type` × `scheduler` cross.

**Coroutine resumptions are measured under a synthetic type.** A resumed coroutine continuation
runs on a cpu worker outside `ActorExecutor` (`CpuWorker::run` calls `h.resume()` directly), so it
gets its own stat slot: `type="td::actor::core::CoroutineResume"` in the per-type families
(`busy_ticks`, `messages`, the maxima; `created`/`alive` stay 0 — there is no actor to count). Once a
resumption returns, its complete outer dispatch is added to both the synthetic type and cpu-worker
totals using the same `Debug` timestamps; there is no second coroutine timer. While it is running,
only the worker snapshot includes its elapsed time. The worker total additionally contains actor
dispatches. The gate is resolved for every resumption, so enabling or disabling stats while workers
are already running takes effect immediately.

---

## ADNL

Two collectors: `AdnlNetworkManager` supplies the wire tier, `Adnl` (the peer table) the transport,
app and query tiers.

### Wire

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_adnl_wire_bytes_total` | counter | `direction=in\|out` | UDP payload bytes the socket actually carried, folded in as deltas during the scrape. Outbound counts only datagrams the kernel accepted — one it refuses is a drop, not wire traffic. |
| `ton_adnl_wire_packets_total` | counter | `direction` | Datagrams the socket actually carried, same source and semantics. |
| `ton_adnl_wire_syscalls_total` | counter | `direction` | Actual UDP send/receive OS calls. An mmsg batch is one call; the fallback counts every per-datagram call. Unsuccessful calls and each user-space retry after `EINTR` count too. Read from `td::UdpServer`'s socket counters and folded in as deltas during the scrape. |
| `ton_adnl_wire_dropped_total` | counter | `direction`, `reason` | `in,limited`: kernel receive-queue overflow (`SO_RXQ_OVFL`, folded in as a delta during the scrape) — these datagrams never reached `wire_packets`. `in,invalid`: packet under 32 bytes. `in,internal`: no callback installed, socket read error, or no `InDesc` for the port. `out,internal`: unknown source id, no matching out rule, or a datagram the kernel refused outright (`EMSGSIZE`/`EACCES`/`EPERM`, folded in as a delta during the scrape). `out,invalid` and `out,limited` are never incremented. |
| `ton_adnl_wire_listening_sockets` | gauge | — | Bound UDP sockets. |

Every ADNL-side inbound drop now fires *after* the socket has already counted the datagram, with one
exception: the socket-read-error site is fed a synthetic message that was never counted in
`wire_bytes`/`wire_packets` at all, so subtracting `in,internal` still over-corrects by that amount.

### Transport

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_adnl_transport_inbound_packets_total` | counter | — | Packets entering the peer table, counted before any routing decision. |
| `ton_adnl_transport_decrypt_packets_total` | counter | — | Packets decrypted **by a local id** and parsed. Channel-decrypted packets bypass this — see Known gaps. |
| `ton_adnl_transport_decrypt_bytes_total` | counter | — | On-wire size of those same packets **minus the 32-byte destination id** that prefixes them. |
| `ton_adnl_transport_dropped_total` | counter | `direction`, `reason=invalid\|limited\|internal` | Peer-table and peer-pair drops. `in,invalid` dominates: short packets, category mismatch, unknown destination, reinit-date and seqno checks (including duplicate/replayed seqnos), bad signature, huge-message reassembly failures. `in,internal`: an unknown peer while the network manager is uninitialized, an unknown destination for a packet we nevertheless decrypted, or an uninitialized peer-pair id. `out,limited` is queue expiry, the 10 MiB queue cap, and `direct_only` messages discarded when no direct route exists. `out,invalid`: unknown source id on send. `out,internal`: building the packet failed, the channel was destroyed mid-send, an empty encryptor, or encryption failed. `in,limited` is never incremented. |
| `ton_adnl_transport_local_ids` | gauge | — | Local ADNL ids. |
| `ton_adnl_transport_peers` | gauge | — | Distinct remote nodes. |
| `ton_adnl_transport_peer_pairs` | gauge | — | `(local_id, peer_id)` pairs. |
| `ton_adnl_transport_channels` | gauge | — | Registered ADNL channels. |
| `ton_adnl_transport_static_nodes` | gauge | — | Static nodes from config. |

### App

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_adnl_app_bytes_total` | counter | `kind=message\|query\|answer`, `direction`, `tl` | Application payload bytes, bucketed by the resolved TL constructor: routing envelopes are unwrapped, up to 4 levels deep, and the innermost constructor wins. Outbound is accounted at the single send choke point, before the huge-message split. |
| `ton_adnl_app_messages_total` | counter | same | Message count for the same events. |
| `ton_adnl_app_dropped_total` | counter | `direction`, `reason` | Payloads refused at the app boundary for size. `out,limited`: the 8 KiB message/query cap, the 1024 B answer MTU, and an answer to an inbound query over the 8 KiB cap. `in,limited`: an inbound answer over the 8 KiB cap, and a huge-message part whose declared `total_size` exceeds it. Only `limited` is ever used, in either direction. |

### Queries

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_adnl_query_duration_seconds` | histogram | `tl`, `le` | Dispatch-to-answer time of an inbound query, measured at the ADNL delivery layer (`AdnlLocalId::deliver_query`) — the single choke point every transport funnels through, so this covers queries arriving over ADNL peer pairs, RLDP2, QUIC and the ext server alike. The clock starts before the subscriber callback is invoked and stops when the answer promise is fulfilled; a promise dropped without an answer counts as a failure with its elapsed time. `tl` comes from the same envelope-resolving logic as the app tier, and magics the schema does not know collapse into `tl="unknown"` so the label space stays bounded by the schema. |
| `ton_adnl_query_failed_total` | counter | `tl` | Of those queries, the ones that answered with an error or dropped their answer promise. |

Validator-engine runs DHT in client mode unless started with `--dht-server`; such nodes deliberately
reject inbound `dht.*` queries with an explicit error. This is usually noise rather than a node-health
failure, not a dropped-promise instrumentation artifact.

A query slower than 1 s also gets an `INFO` log line with its `tl` name, the other end's id under
`peer=` (the source here, the destination for the outbound families below) and the elapsed time — no
payload size. The throttle is per bucket, one line per 10 s per site, since losing a peer makes every
one of its queries slow at once. These logs are a temporary diagnostic, to be removed once the
latency histograms are trusted.

### Outbound: roundtrips and deliveries

The outbound mirror is **per transport**, measured where the transport accepts the send:

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_adnl_query_roundtrip_seconds` / `ton_rldp2_…` / `ton_quic_…` | histogram | `tl`, `le`; QUIC also `trust` | Transport-accept to answer for queries we send: network + peer processing + transfer time. Errors and timeouts land in the matching `…_query_roundtrip_failed_total` with the same non-`le` labels. |
| `ton_rldp2_message_delivery_seconds` / `ton_quic_…` | histogram | `tl`, `le`; QUIC also `trust` | Transport-accept to the protocol's receipt confirmation for fire-and-forget messages: RLDP2 confirms via the transfer's completion (`on_sent`) and measures only sends carrying a timeout; QUIC uses the empty response the receiver answers every message with. Failures land in the matching `…_message_delivery_failed_total` with the same non-`le` labels. |

Note the asymmetry: inbound `ton_adnl_query_duration_seconds` covers queries from **all** transports
at the single delivery layer, while roundtrip/delivery are per-transport at the sending layer. Plain
ADNL messages have no delivery metric — a UDP datagram has no acknowledgement. Slow roundtrips and
deliveries (>1 s) get the same throttled `INFO` log treatment as slow inbound queries.

**Peer-pair accounting.** `Counter` is a plain non-atomic integer, so per-peer-pair counters cannot
be bumped cross-thread. Each pair accumulates locally; on scrape the peer table asks every pair to
`drain` (a destructive `std::exchange`) and merges the delta into the process-wide aggregate. A pair
destroyed between scrapes drains from `tear_down()`, so its final counts are not lost. **All exposed
ADNL numbers are process-wide** — there is no per-peer or per-local-id label anywhere.

---

## QUIC

One collector on `QuicSender`, which fans out across all `QuicServer` instances (one per bound
port) and folds their stats together.

### Wire

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_quic_wire_bytes_total` | counter | `direction` | UDP payload bytes. Inbound counts each recvmmsg message, i.e. the whole GRO super-buffer. |
| `ton_quic_wire_packets_total` | counter | `direction` | Datagrams after GRO/GSO segmentation, so bytes-per-packet is only meaningful in aggregate. |
| `ton_quic_wire_syscalls_total` | counter | `direction` | Actual UDP send/receive OS calls. An mmsg batch is one call; the fallback counts every per-datagram call. Empty or failed calls and each user-space retry after `EINTR` count too. |
| `ton_quic_wire_dropped_total` | counter | `direction`, `reason` | `in,limited`: kernel receive-queue overflow (read as a delta from `SO_RXQ_OVFL` **during the scrape**) — never counted in `wire_packets`. `in,invalid`: per-message socket errors (truncated or otherwise malformed datagrams). `out,limited`: a stateless datagram the socket's send queue refused; stateless packets are not retried. `out,internal`: a datagram the kernel refused outright (`EMSGSIZE`/`EACCES`/`EPERM`), or a stateless send that failed with an error. Blocked connection egress *is* retried and is not counted here. `in,internal` and `out,invalid` are never incremented. |
| `ton_quic_wire_listening_sockets` | gauge | — | Distinct bound UDP ports. |

### Transport

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_quic_transport_connections_total` | counter | `direction` | Connections ever installed, including ones that never completed the handshake. `direction` is who dialled — `in` counts a peer's first datagram to us, `out` counts a connection we opened — so this is the only place inbound *attempts* are visible, whereas `handshakes` sees only the ones that reached a verdict. |
| `ton_quic_transport_connections_current` | gauge | `direction` | Connections currently installed, by who dialled. A connection is installed on its first datagram, so this **includes** the ones still handshaking, not only the ready ones. |
| `ton_quic_transport_connections_ready` | gauge | `direction`, `trust` | Authenticated `QuicSender` paths ready for application traffic, counted once per local/peer identity pair rather than per physical connection ID. `trusted` is a local resource class: at least one live permanent-overlay registration exists for the path on that sender (normally a validator peer on validator overlays). Eager-only and unregistered paths are `untrusted`; this is not an authorization decision. Trust is evaluated on every scrape, so registration changes reclassify a live path immediately. Raw `QuicServer` users do not contribute. |
| `ton_quic_transport_bytes_total` | counter | `direction` | ngtcp2 packet bytes. |
| `ton_quic_transport_packets_total` | counter | `direction` | ngtcp2 packet count. |
| `ton_quic_transport_stream_bytes_total` | counter | `direction` | STREAM payload. Inbound at delivery; **outbound at ACK time**, so it trails the app tier by everything in flight or lost. |
| `ton_quic_transport_bytes_lost_total` | counter | — | Bytes in packets declared lost by loss detection. |
| `ton_quic_transport_packets_lost_total` | counter | — | Packets declared lost. |
| `ton_quic_transport_bytes_in_flight` | gauge | — | ngtcp2 bytes in flight. |
| `ton_quic_transport_bytes_unacked` | gauge | — | Stream bytes appended but not yet acked, so it **includes** `bytes_unsent` — the two are not disjoint. |
| `ton_quic_transport_bytes_unsent` | gauge | — | App-buffered stream bytes not yet handed to ngtcp2. |
| `ton_quic_transport_sids_total` | counter | — | **Peer-initiated** bidi streams accepted. Locally opened streams are not counted. |
| `ton_quic_transport_sids_current` | gauge | — | Open streams, counting both directions of initiation. |
| `ton_quic_transport_mean_rtt_seconds` | gauge | — | Connection-weighted mean smoothed RTT over open connections. |
| `ton_quic_transport_dropped_total` | counter | `direction`, `reason` | `in,invalid`: unroutable datagram, invalid Retry token, protocol violation, a handshake rejected over a key or identity mismatch, plus ngtcp2's own discarded-packet delta. `in,limited`: per-IP flood limiter, or a handshake rejected because the path's MTU is 0. `in,internal`: connection creation failure, failing to build a stateless Retry, a fatal ngtcp2 error while handling ingress (our own OOM or callback failure), or a handshake rejected because the outbound connection it belongs to is no longer known. `out,internal`: egress production failure. `out,invalid` and `out,limited` are never incremented. To avoid double-counting, a refused datagram is counted here only if `pkt_discarded` did not move across that `ngtcp2_conn_read_pkt` call. Because ngtcp2 exposes no per-packet attribution, a rare buffered-packet interleaving can undercount by one; see *Known gaps*. Failing to *send* a Retry or a stateless close is an egress drop rather than an inbound reject. Rejected handshakes are counted by whoever rejects them — synchronously at the callback (a key that will not parse, always `invalid`), or asynchronously by the actor that deferred its verdict, which supplies the reason — so they are **not** uniformly `invalid`. |
| `ton_quic_transport_handshakes_total` | counter | `direction`, `result` | Handshakes that reached the application's verdict, split by who dialled (`in` = the peer dialled us, `out` = we dialled the peer — a rejection means something quite different on each side) and how it went: `completed` once the connection is ready to carry traffic, `rejected` when the application refused the peer (a key that will not parse, an identity that does not match the one we dialed, a path with no usable MTU, an outbound connection nobody remembers). The two are disjoint, and every rejection also lands in `dropped{direction="in"}` under its reason — `dropped`'s `direction` is the direction of the discarded data, not of the dial, so an outbound handshake we reject shows up as `handshakes{direction="out"}` against `dropped{direction="in"}`. A handshake abandoned before the application ever saw it is counted in neither: an idle timeout mid-handshake only removes the connection, so it shows up as a decrement of `connections_current` and nowhere else, while a datagram ngtcp2 refused lands in `dropped`. Only consumers built on `QuicSender` report completions — a callback implemented directly against `QuicServer` (the in-tree examples and raw tests) records rejections but not successes. |

### App

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_quic_app_bytes_total` | counter | `trust`, `kind`, `direction`, `tl` | Inner ADNL payload bytes carried over QUIC streams, measured outside the `quic_message`/`quic_query`/`quic_answer` wrapper. Inbound answers are counted when they successfully complete the matching local query, not merely when an answer frame reaches the wire callback. |
| `ton_quic_app_messages_total` | counter | same | Message count. |
| `ton_quic_app_dropped_total` | counter | `trust`, `direction`, `reason` | Fire-and-forget message sends that failed: `out,limited` when the peer's stream-count credit blocked opening a stream (`NGTCP2_ERR_STREAM_ID_BLOCKED`), `out,internal` for any other send failure. Query failures are not counted here — they propagate to the caller. Inbound cells are never incremented. |

`trust` is the local resource class assigned to the remote local/peer identity path, not an
authorization result.
QuicSender snapshots it once when a logical query or message starts and uses that value for the
request, answer or drop, and latency outcome. A registration change therefore affects new traffic
only; existing counter history stays in its original class, while `connections_ready` reclassifies
immediately on the next scrape. The label has two closed values and no peer id, so its cardinality is
bounded. Socket, pre-auth, and aggregated transport metrics have no `trust` label because peer
identity is not available at every accounting point.

Adding this label changes the Prometheus series identity once at rollout. Queries that aggregate
the family continue to work; consumers of raw QUIC app/latency series must select or retain
`trust`. A matcher such as `{trust=~".*"}` also includes an unlabeled series during a rolling
upgrade, while selecting `trusted` or `untrusted` intentionally excludes old exporters.

### Outbound latency

Described in full under ADNL → *Outbound: roundtrips and deliveries*; the QUIC families are:

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_quic_query_roundtrip_seconds` | histogram | `trust`, `tl`, `le` | Send-accept to answer for queries we send over QUIC. Connection setup is deliberately outside the measured window. |
| `ton_quic_query_roundtrip_failed_total` | counter | `trust`, `tl` | Of those, the ones that errored or timed out. |
| `ton_quic_message_delivery_seconds` | histogram | `trust`, `tl`, `le` | Send-accept to the empty response the receiver answers every fire-and-forget message with. |
| `ton_quic_message_delivery_failed_total` | counter | `trust`, `tl` | Of those, the ones that never got their confirmation (including a connection closing with messages in flight). |

---

## RLDP2

One collector on `Rldp`. Per-connection counters are drained into an aggregate the same way ADNL
drains peer pairs; a dropped connection drains from `tear_down`, so counts survive connection churn.
Removal is not an idle timeout: every use pushes the connection's deadline to
`max(current deadline, this transfer's deadline + 120 s)`, so a transfer due at T holds its
connection until T + 120 s. Two sites bypass the per-connection drain and write the aggregate
directly, having no connection to attribute to: the wire bytes of a datagram from a peer with no
permitted connection, and a reassembled message that fails to parse.

### Wire

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_rldp2_wire_bytes_total` | counter | `direction` | Bytes of RLDP2 datagrams exchanged with ADNL. |
| `ton_rldp2_wire_packets_total` | counter | `direction` | Count of those datagrams. |

### Transport

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_rldp2_transport_transfers_total` | counter | `direction`, `state=completed\|failed\|timeout` | Terminal outcome of a whole transfer (a reassembled message, not a packet); errors carrying `ErrorCode::timeout` are classified as `timeout`, everything else as `failed`. |
| `ton_rldp2_transport_connections` | gauge | — | Live connections. |
| `ton_rldp2_transport_queries_pending` | gauge | — | Outbound queries awaiting an answer. |
| `ton_rldp2_transport_dropped_total` | counter | `direction`, `reason` | Protocol-layer rejects: malformed TL, bad FEC type or symbol size, bad seqno, a declared `total_size` too large for a `size_t`, part index or size mismatch, undecodable FEC symbol (`invalid`); transfer over the size cap (`limited`). All are inbound; `out` and `internal` are always 0. |

### App

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_rldp2_app_bytes_total` | counter | `kind`, `direction`, `tl` | Payload bytes crossing the RLDP↔app boundary. |
| `ton_rldp2_app_messages_total` | counter | same | Message count. |
| `ton_rldp2_app_dropped_total` | counter | `direction`, `reason` | `in,limited`: datagram from a peer with no permitted connection, or an inbound answer exceeding our query's `max_answer_size`. `out,limited`: our answer exceeded the requester's `max_answer_size`. |

### Outbound latency

Described in full under ADNL → *Outbound: roundtrips and deliveries*; the RLDP2 families are:

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_rldp2_query_roundtrip_seconds` | histogram | `tl`, `le` | Send-accept to answer for queries we send over RLDP2. |
| `ton_rldp2_query_roundtrip_failed_total` | counter | `tl` | Of those, the ones that errored or timed out. |
| `ton_rldp2_message_delivery_seconds` | histogram | `tl`, `le` | Send-accept to the transfer's completion (`on_sent`) for fire-and-forget messages. Only sends that carried a timeout are measured: without one the transfer gets no deadline and `on_sent` may never fire, so those messages are left out entirely rather than counted as never delivered. |
| `ton_rldp2_message_delivery_failed_total` | counter | `tl` | Of those, the ones whose transfer never completed. |

---

## Overlay

One collector on `Overlays` (the overlay manager). Records broadcast **content** — the naked TL
payload, after FEC reassembly — where the transport tiers can only see wire bytes and FEC parts.
Outbound is counted on the manager at the send choke points; inbound accumulates per-overlay actor
and is drained into the manager's aggregate on scrape (the same drain/absorb idiom as ADNL peer
pairs), with a `tear_down` flush so a dying overlay's counts survive.

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_overlay_broadcast_bytes_total` | counter | `direction=in\|out`, `tl` | Broadcast content bytes. `out` at the four terminal `send_broadcast*` entry points, pre-FEC-encoding, and only for content submitted to an overlay this node participates in (still counted if certificate checks later reject it). `in` at `deliver_broadcast`, post-reassembly. |
| `ton_overlay_broadcast_messages_total` | counter | same | Broadcast count for the same events. |

Two semantics worth knowing. Sizes here are content bytes while the transport tiers count wire
bytes, so the transport app tier's FEC-part traffic (`*_app_*` under the four FEC constructors:
`overlay.broadcastFec`, `overlay.broadcastFecShort`, `overlay.broadcastPlumtreeFec`,
`overlay.broadcastTwostepFec`) minus this tier's inbound bytes prices the FEC redundancy and
duplicate reception. Only the FEC constructors stay coarse: simple `overlay.broadcast`,
`overlay.broadcastPlumtreeSimple` and `overlay.broadcastTwostepSimple` are unwrapped through to
their content, so they land under the content's own `tl` in the app tier. And `direction="in"`
includes self-originated broadcasts — a node delivers its own broadcast to its own callbacks, so a
locally originated broadcast increments both directions; `in` means "content this overlay
delivered", not "received from peers".

---

## Validator manager

From `ValidatorManagerImpl`. All of these sit directly under the root prefix with no subsystem
segment (except the `ton_mempool_*` families, whose segment is `mempool`), unlike every other family.
The two block-receive families come first and are unchanged:

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_first_received_total` | counter | `workchain=0\|-1`, `source` | For each block that reaches "applied", one increment for the **earliest** source that delivered it. |
| `ton_received_total` | counter | `workchain`, `source` | For each applied block, one increment **per distinct source** that delivered it, including sources that arrived after the block was applied — but only as long as the block is still in the 1000-entry LRU of receive stats, so a source that turns up much later is missed. |

`source` values: `unknown`, `block_broadcast_public`, `block_broadcast_fast_sync`,
`block_broadcast_custom`, `block_download`, `candidate_broadcast_public`,
`candidate_broadcast_fast_sync`, `candidate_broadcast_consensus`, `candidate_broadcast_custom`,
`candidate_finality_public`, `candidate_finality_fast_sync`, `candidate_finality_custom`,
`candidate_stored`, `block_accepted`.

Cardinality is fixed at 2 × 14 = 28 series per family; `workchain` is a closed two-value domain,
not a dynamic label. Both families are gated on the manager having started (`started_`), so blocks
applied during initial sync are not counted at all.

### Sync

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_masterchain_seqno` | gauge | — | Seqno of the last **applied** masterchain block. No samples until the first MC block is applied, so during initial sync the family renders only its `# TYPE` line. |
| `ton_masterchain_block_age_seconds` | gauge | — | Local system clock minus that block's `unix_time`. This compares two different clocks, so clock skew — ours or the block producer's — shows up here, including as small negative values; sustained growth is the sync-lag signal. Same no-samples-until-applied guard as `seqno`. |
| `ton_shardclient_seqno` | gauge | — | Seqno of the masterchain block up to which the shard client has processed shards. Read from the handle the shard client already mirrors into the manager, so collection does not wait on the child actor. No sample until that mirror exists. `masterchain_seqno − shardclient_seqno` is the shard-processing lag in blocks. |
| `ton_active_shards` | gauge | — | Active non-masterchain leaf shards in the latest applied masterchain state. No sample until that state is available. |

Block rate is a property of the selected chain, not a value to sum or normalize by validator count.
For masterchain, aggregate the replicated head first and then calculate its advance:

```promql
clamp_min(
  (max(ton_masterchain_seqno{job=~"$job"})
    - max(ton_masterchain_seqno{job=~"$job"} offset 1m)) / 60,
  0
)
```

The selected jobs must belong to one blockchain; a maximum cannot reconcile unrelated networks.
The canonical dashboards use this head only when every reporter of the older applied-block stream
also exports `ton_masterchain_seqno`. During a rolling upgrade they fall back to the reporter median
instead of letting one new but stalled target suppress legacy observations.

Shardchain has no single global seqno. Sum `ton_first_received_total{workchain="0"}` over `source`
per node, then use a median only to reconcile duplicate observations from healthy nodes. A
per-active-shard rate should divide each node's one-minute block rate by
`avg_over_time(ton_active_shards[1m])`, so a split or merge uses the same window in numerator and
denominator. Chain-level block-rate panels should ignore instance selectors; per-node differences
belong in sync and propagation diagnostics.

`ton_applied_ext_messages_total` is different from these block-rate signals: it includes replay
while a node catches up. Dashboard external-flow panels take a median only across reporters whose
absolute masterchain age is below 120 seconds; shardchain observations additionally require
shard-client lag of at most two blocks. This rejects the usual catch-up path and future-skewed chain
clocks, and limits a single reporter's spike, but it does not turn the counter into protocol-wide
chain truth.

### Validator

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_collated_blocks_total` | counter | `chain=master\|shard`, `result=ok\|error` | Block collations attempted by this node, by outcome. Process-lifetime: resets to 0 on restart. All four cells are always emitted; on a node that does not collate they stay 0. |
| `ton_validated_blocks_total` | counter | same | Block validations (candidate checks) by this node, same semantics; on a node that does not validate the cells stay 0. |
| `ton_block_processing_seconds_total` | counter | `operation=collate\|validate`, `chain=master\|shard`, `result=ok\|error`, `phase`, `clock=elapsed\|real\|cpu` | Seconds accumulated in existing per-block timing statistics. `phase="total",clock="elapsed"` is end-to-end time; `real` and `cpu` expose instrumented work phases. Process-lifetime, reset on restart. |
| `ton_collation_ext_messages_total` | counter | `chain=master\|shard`, `result=ok\|error`, `outcome=filtered\|skipped_backpressure\|included\|rejected` | External messages dequeued by collation attempts. Outcomes partition the messages considered by each attempt; discarded automatic retries use `result="error"`. `included` means execution succeeded in that attempt, not that the message was applied on-chain; use `result="ok"` for completed candidates. Per-collator events: exactly one node emits per attempt, so `sum()` over the fleet is the chain-wide rate. |
| `ton_collation_transactions_total` | counter | `chain=master\|shard` | Transactions in successfully collated candidates. |
| `ton_collation_gas_total` | counter | same | Gas used by successfully collated candidates. |
| `ton_collation_block_bytes_total` | counter | same | Serialized bytes in successfully collated candidates. |
| `ton_collation_collated_data_bytes_total` | counter | same | Collated-data bytes in successfully collated candidates. |
| `ton_collation_ext_messages_offered_total` | counter | same | External messages offered to successful final collation attempts. |
| `ton_collation_want_split_total` | counter | `chain=master\|shard` | Successful final collation attempts whose resulting block set `want_split`. This is the decision from weighted overload history, not necessarily a condition caused by the current block. |
| `ton_collation_overload_total` | counter | `chain`, `reason=block_limits\|out_msg_queue\|long_collation\|dispatch_queue\|unknown` | Successful final collation attempts whose current block contributed an overload-history bit, by its selected cause. No increment for a block with no current contribution. |
| `ton_applied_ext_messages_total` | counter | `chain=master\|shard` | Inbound external-message records observed in blocks applied by this node, including catch-up replay. This is the on-chain stage, independent of whether this node produced the candidate. A process-local recent-block cache suppresses duplicate counting; duplicate requests still repeat the idempotent pool cleanup. A fleet sum counts the same chain traffic once per reporter. Reconcile synchronized reporters with a median, never a sum; even then, a brief post-catch-up burst can remain in the rate window, so this is a replicated observation rather than an objective chain counter. |
| `ton_validator_groups` | gauge | `chain` | Validator groups this node currently participates in. Samples only while the node is a validator with a computed network state — a fullnode emits the family with no samples, so absence means "not validating", not 0. |

Most timing samples reuse statistics already collected for validator session logs. Four broad scopes
cover previously unattributed collation work: `dispatch_queue`, `import_internals`, `import_externals`,
and `process_new_msgs`. They include nested transaction work, so they overlap `trx_tvm`,
`trx_storage_stat`, and `trx_other`. Collation exports those 17 work phases plus `wait_externals`;
validation exports its 19 work phases plus `active` and `waiting`.
Timing samples for an operation/chain/result tuple appear only after its first attempt; all applicable phase/clock
samples are emitted thereafter, including zeros. Split and overload cells are emitted from boot, including zeros.
Only valid phase/clock pairs are emitted: work-time phases have `real` and `cpu`; `total` also has
end-to-end `elapsed`; `wait_externals`, `active`, and `waiting` are elapsed observations.

These phases are attribution signals, not a partition of elapsed time. Instrumented scopes can nest
or overlap, and account validation can run in parallel, so phase sums — especially validation real
or CPU work — may exceed end-to-end elapsed time. Error-phase timings are safe but best-effort: completed
scopes are recorded, while a scope still active when failure is reported may be omitted. Collation reports
only the final attempt. Likewise, plot `want_split` separately from overload reasons: `want_split` reflects
weighted history, while an overload reason identifies only the current block's contribution to that history.

For collation externals, `filtered` failed registration, `skipped_backpressure` was left pending because
the outbound queue was large, and `rejected` did not make it into that candidate, normally because the
TVM rejected it (for example, an earlier included copy already advanced the seqno) or because processing
aborted the attempt. Execution attempts are the sum of `included` and `rejected`.
Unlike the timing family, external outcomes include discarded intermediate retry attempts. All 16
chain/result/outcome cells and both applied-message cells are emitted from boot, including zeros.

The five collation-work families count only successful final attempts. Sum them across collators and
divide by `rate(ton_collated_blocks_total{result="ok"})` for per-block values. Both chain cells in each
family are emitted from boot.

### Mempool

Read on demand from `ExtMessagePool` as one small value snapshot. Like the ADNL, RLDP2 and overlay
collectors, the validator-manager collector waits for its child actor before it emits the families.
Values are therefore current for that scrape rather than cached. If the pool disappears while the
request is in flight, only the mempool and applied-external families are omitted; the rest of the
scrape still succeeds.

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_mempool_ext_messages` | gauge | — | External messages currently pending in the mempool, summed over all priority levels. Includes postponed (temporarily inactive) messages and expired ones the periodic cleanup has not swept yet (messages live 600 s, the sweep runs every 250 s). |
| `ton_mempool_oldest_ext_message_age_seconds` | gauge | — | Age of the oldest current mempool entry, maintained without scanning the pool. Includes postponed and expired-unswept messages; 0 when empty. Reprioritizing a duplicate recreates the entry and resets its age, matching its expiry behavior. |
| `ton_mempool_ext_admission_total` | counter | `outcome=accepted\|not_ready\|too_large\|backpressure\|invalid\|state_unavailable\|vm_rejected\|rate_limited\|pool_full\|address_full\|duplicate\|internal_error\|reprioritized` | One local outcome for every external handed to the validator manager or pool. `accepted` passed validation and, on a node that stores externals, was inserted as a new pool entry. `reprioritized` passed validation and replaced its own lower-priority entry. On nodes that store externals, `accepted` minus removals tracks the pending gauge; `reprioritized` changes neither side. `rate_limited` is the final per-address validation cap. `pool_full`, `address_full`, and `duplicate` passed validation but were not inserted locally; they do not change the existing successful network response. Raw errors, addresses, and VM exit codes are never labels. Process-lifetime; all cells are emitted from boot. |
| `ton_mempool_ext_check_total` | counter | `result=ok\|error` | External-message admission checks that ran, by outcome. `error` is a failed check (parse, account state fetch, VM) or the per-address cap at finalization; requests rejected **before** a check runs — node not ready, oversized payload, admission queue full — are counted in neither cell. Process-lifetime, resets on restart. |
| `ton_mempool_ext_removed_total` | counter | `reason=applied\|expired\|rejected_final\|filtered\|pool_pressure` | Why an entry left this node's pool. `applied` — seen in an applied block; `expired` — hit the 600 s TTL and was swept; `rejected_final` — exhausted its postpone generations; `pool_pressure` — was evicted instead of postponed while its priority level was at the soft limit; `filtered` — collation could not register it, for example because it was duplicate or for the wrong shard. A non-`applied` removal is a local eviction, not proof the message was lost network-wide. Reprioritization is not a removal; it is admission outcome `reprioritized`. Process-lifetime; all cells emitted from boot. |

Admission starts at `ValidatorManager`: malformed outer broadcasts, unauthorized custom-overlay senders,
inactive overlays, and duplicates rejected by the public overlay never reach this boundary. `duplicate` therefore
means a duplicate that reached the pool. Pool storage outcomes do not change the existing network response: for
example, an already-known message can still be allowed to propagate while being counted as `duplicate` locally.

---

## The data ladder

`metrics/well-known.h` documents an intended tier relationship: wire bytes, minus kernel-queue
drops, minus transport drops, gives useful bytes; minus protocol overhead gives stream bytes; minus
ADNL wrapping gives app bytes. The middle `stream_bytes` rung is implemented for QUIC
(`ton_quic_transport_stream_bytes_total`); nothing emits the `useful` or `stateless_dropped` rungs
today.

What you can actually relate, approximately:

```
ton_adnl_wire_packets_total{direction="in"}
  − ton_adnl_wire_dropped_total{direction="in",reason="invalid"}    under 32 bytes
  − ton_adnl_wire_dropped_total{direction="in",reason="internal"}   no InDesc / no callback
≈ ton_adnl_transport_inbound_packets_total
  − ton_adnl_transport_dropped_total{direction="in"}                routing and packet-check rejects
  − (unmetered: rate limiter, decrypt failures, per-IP peer cap)
≈ ton_adnl_transport_decrypt_packets_total                          non-channel packets only
  − ADNL framing overhead
≈ ton_adnl_app_bytes_total
```

Subtract only `invalid` and `internal`: `reason="limited"` at the wire tier is kernel receive-queue
overflow, and those datagrams never reached `wire_packets` to begin with. The ADNL wire tier reads `wire_bytes`/`wire_packets` from the socket
itself, so a datagram the kernel refused on send is counted once, as a drop, and never as
transmitted traffic. (QUIC computes its wire counts from its own send/receive batches instead.)

The `≈` are real. Tiers are sampled at different instants within one scrape, counts are taken at
different points in a packet's life, and several drop paths are unmetered.

---

## Useful queries

Recipes for the questions this surface was built to answer. Rules that keep histogram math honest:
`le` must survive every `by (…)` clause, always `rate()` bucket counters before quantiles, and when
aggregating across nodes sum the bucket rates *before* `histogram_quantile` — a p95 of per-node
p95s is not a p95. Quantiles are interpolated within our fixed bucket bounds (1 ms … 30 s,
log-scale), so read "p95 = 8.3ms" as "p95 is in the 5–10 ms bucket".

**What am I receiving, by type and QUIC peer class?** Query one transport at a time:

```promql
topk(10, sum by (trust, tl) (rate(ton_quic_app_messages_total{kind="query",direction="in"}[5m])))
```

Swap `messages`→`bytes` for traffic share; to fold in response volume (answers usually dominate
bytes) widen to `kind=~"query|answer"` — `kind` is an equality matcher, so a second `kind="answer"`
would match nothing rather than add a case.

**Do not reach for `{__name__=~"ton_(adnl|quic|rldp2)_app_bytes_total"}` to do all three at once.**
`rate()` drops `__name__`; ADNL and RLDP2 then have the same label set and collide, so Prometheus
fails the whole query with `vector cannot contain metrics with the same labelset`. QUIC's extra
`trust` label does not repair that collision. Nothing after `rate()` can repair it —
`label_replace` runs too late. If you want a cross-transport total, synthesize the distinguishing
label in a recording rule, where each `rate()` is evaluated separately (this example deliberately
aggregates QUIC trust classes):

```yaml
- record: ton:app_bytes:rate5m
  expr: sum by (transport, kind, direction, tl) (
          label_replace(rate(ton_quic_app_bytes_total[5m]),  "transport", "quic",  "", "")
       or label_replace(rate(ton_adnl_app_bytes_total[5m]),  "transport", "adnl",  "", "")
       or label_replace(rate(ton_rldp2_app_bytes_total[5m]), "transport", "rldp2", "", ""))
```

**What content am I gossiping?** Broadcast content, post-FEC-reassembly:

```promql
topk(10, sum by (tl) (rate(ton_overlay_broadcast_bytes_total{direction="in"}[5m])))
```

**Traffic and overhead per transport** — wire versus app tells you what the transport costs:

```promql
sum by (direction) (rate(ton_quic_wire_bytes_total[5m]))
  / sum by (direction) (rate(ton_quic_app_bytes_total[5m]))
```

Same shape for `adnl` and `rldp2` (measured on a small mixed workload: ADNL ≈ 1.3×, QUIC ≈ 1.8× —
small-message-heavy flows pay more per-packet overhead). Remember ADNL's app tier includes rldp2's
datagrams (`tl="rldp2.messagePart"`), so don't sum adnl and rldp2 app tiers together.

**FEC tax**: transport-tier FEC part bytes versus reassembled content bytes:

```promql
  sum(rate(ton_quic_app_bytes_total{direction="in",
            tl=~"overlay.broadcast(Fec|FecShort|PlumtreeFec|TwostepFec)"}[5m]))
/ sum(rate(ton_overlay_broadcast_bytes_total{direction="in"}[5m]))
```

One transport per query, for the labelset reason above; add ADNL's share via the recording rule if
broadcasts also arrive over ADNL. Both sides must be inbound-only: the denominator is, so a
numerator without `direction="in"` mixes in what we forwarded and inflates the tax. The `tl` alternation lists every FEC-part constructor the
schema has — miss one and its bytes silently vanish from the numerator.

This is not an exact ratio: the denominator is *all* delivered broadcast content, including
simple-broadcast and self-originated bodies that cost no FEC parts, so the quotient reads as FEC part
bytes over all broadcast content and understates the real per-byte FEC overhead — it only approaches
it on FEC-dominated traffic such as block propagation. Isolating FEC-delivered content exactly would
need a delivery-mode label on the overlay tier, deliberately not added for cardinality.

**How long do we take to answer** — inbound processing p95 per query type, and its error ratio:

```promql
histogram_quantile(0.95, sum by (tl, le) (rate(ton_adnl_query_duration_seconds_bucket[5m])))

sum by (tl) (rate(ton_adnl_query_failed_total[5m]))
  / sum by (tl) (rate(ton_adnl_query_duration_seconds_count[5m]))
```

**Is it us or the network?** Compare inbound processing against outbound roundtrip for the same
type — processing is our cost, roundtrip adds network + the peer:

```promql
histogram_quantile(0.95, sum by (le) (rate(ton_rldp2_query_roundtrip_seconds_bucket{tl="tonNode.downloadBlockFull"}[5m])))
histogram_quantile(0.95, sum by (le) (rate(ton_adnl_query_duration_seconds_bucket{tl="tonNode.downloadBlockFull"}[5m])))
```

**Are my messages actually arriving?** Delivery confirmation failure ratio (rldp2 confirms via
transfer completion, QUIC via the empty response) — on a healthy link this is ~0 and deliveries
confirm in milliseconds; a peer that silently lost its connection state shows up here within
seconds:

```promql
sum by (trust, tl) (rate(ton_quic_message_delivery_failed_total[5m]))
  / sum by (trust, tl) (rate(ton_quic_message_delivery_seconds_count[5m]))
```

**QUIC stream-credit exhaustion** (the `ngtcp2_conn_open_bidi_stream failed: -206` signature —
fire-and-forget sends being dropped because a peer stopped granting stream credit):

```promql
rate(ton_quic_app_dropped_total{direction="out",reason="limited"}[1m]) > 0
ton_quic_transport_sids_current   # corroborates; read it with the caveat below
```

The first line is the detector. `sids_current` only corroborates: it counts open streams in **both**
directions of initiation, each capped at 4096, so a connection's ceiling is 8192 and the gauge cannot
isolate the half that matters. The credit that blocks our sends is the peer's limit on the
locally-initiated half, so a plateau near 4096 × connections is the exhaustion signature only while
inbound stream use is low.

**Why am I dropping traffic?** The reason axis separates runbooks — `limited` on the wire tier is
kernel receive-queue overflow (raise `SO_RCVBUF` / add CPU), `invalid` is garbage from peers,
`internal` is our own failure:

```promql
sum by (reason) (rate(ton_adnl_wire_dropped_total{direction="in"}[5m]))
sum by (reason, direction) (rate(ton_adnl_transport_dropped_total[5m]))
```

**Where do my blocks come from?** Share of first delivery per source (a validator should see
`candidate_broadcast_consensus` dominate; a fullnode, the broadcast/fast-sync paths):

```promql
sum by (source) (rate(ton_first_received_total[15m]))
  / ignoring(source) group_left sum(rate(ton_first_received_total[15m]))
```

**How much CPU goes into crypto?** Signing and verification rates, and their average cost:

```promql
sum by (op) (rate(ton_perf_ops_total{op=~"Ed25519_.*"}[5m]))
rate(ton_perf_op_ticks_total{op="Ed25519_verify_signature"}[5m])
  / rate(ton_perf_ops_total{op="Ed25519_verify_signature"}[5m])
```

A verification rate far above the signing rate means inbound work — block signature sets, DHT and
overlay traffic — rather than our own block production; a sudden jump with no matching block rate is
the shape of a signature-flood.

**How busy is the actor framework, and who is making it busy?** The first expression is scheduler
group occupancy, in busy workers per available actor worker. The second names the classes receiving
the most message-handler or coroutine-resumption time:

```promql
sum(
  rate(ton_actor_worker_busy_ticks_total{worker=~"io|cpu"}[5m])
  / on (job, instance) group_left max by (job, instance) (ton_actor_ticks_per_second)
) / sum(ton_actor_worker_threads{worker=~"io|cpu"})
topk(10, sum by (type) (
  rate(ton_actor_busy_ticks_total[5m])
  / on (job, instance) group_left max by (job, instance) (ton_actor_ticks_per_second)
))
```

This is worker occupancy, not OS CPU utilisation: blocking and descheduling inside a dispatch are
included, while work on rocksdb background threads and the collator pool is outside both numerator
and denominator. The per-type attribution excludes signal-only work and dispatch bookkeeping, so it
does not sum exactly to the worker total.

**Which kind of worker is saturated?** The combined group ratio above blurs two very different pools:
each scheduler has exactly one io worker (every polling actor — all the sockets — is pinned to it)
and a pool of cpu workers. Per-kind utilisation, in busy cores per available thread:

```promql
sum(rate(ton_actor_worker_busy_ticks_total{worker="io"}[5m])
  / on (job, instance) group_left max by (job, instance) (ton_actor_ticks_per_second))
  / sum(ton_actor_worker_threads{worker="io"})
sum(rate(ton_actor_worker_busy_ticks_total{worker="cpu"}[5m])
  / on (job, instance) group_left max by (job, instance) (ton_actor_ticks_per_second))
  / sum(ton_actor_worker_threads{worker="cpu"})
```

An io ratio approaching 1 is the sharper signal: that single thread cannot be added to, only
relieved — the io worker is unsplittable, so the fix is moving work off polling actors, not more
threads. Coroutine resumptions contribute their outer dispatch to the cpu ratio;
`type="td::actor::core::CoroutineResume"` provides the separate per-type resumption attribution
(see *Coroutine resumptions* in the Actors section).

**Is an actor class leaking?** Live count, and the classes whose count only ever grows:

```promql
topk(10, ton_actor_alive)
topk(10, deriv(ton_actor_alive[1h]) > 0)
```

`deriv` over an hour is deliberately slack — most classes are legitimately spiky (one actor per
query, per connection, per download), so a leak is a class whose floor rises across hours, which is
what a positive derivative over a long window catches and a `> N` threshold does not.

**What blocks a worker?** The longest timed executor drain per class over the two recent TSC buckets.
It covers signal and mailbox processing but excludes finish-time bookkeeping, so it is a useful
head-of-line-blocking indicator rather than an exact measurement of the full actor lock hold:

```promql
topk(10, ton_actor_max_execute_ticks
  / on (job, instance) group_left max by (job, instance) (ton_actor_ticks_per_second))
topk(10, ton_actor_max_message_ticks
  / on (job, instance) group_left max by (job, instance) (ton_actor_ticks_per_second))
```

**Is the scheduler behind?** Queue depth now, and how long messages have been waiting to start:

```promql
ton_actor_scheduler_local_queue_length
topk(10, ton_actor_max_queue_ticks
  / on (job, instance) group_left max by (job, instance) (ton_actor_ticks_per_second))
```

A local queue length that is nonzero on most scrapes means the cpu workers are saturated; a large
converted `max_queue_ticks` with an empty queue means a single long batch (see above) delayed everyone once.
Both read only the per-worker queues — see *Known gaps*.

**Is a worker stuck right now?** Point sample of the longest in-flight execution per scheduler:

```promql
ton_actor_scheduler_current_execute_seconds > 5
```

Alert on it only with a `for:` clause of several scrapes: a legitimately long batch (state
serialization, a big collation) will trip a single sample. If it stays high while
`ton_exporter_last_collection_timestamp_seconds` keeps advancing, the wedged worker is on another
scheduler than the exporter's.

**Is the exporter itself healthy?** Scrape staleness — alerts if collection wedges (there is no
internal scrape deadline; see Known gaps):

```promql
time() - ton_exporter_last_collection_timestamp_seconds > 120   # answering scrapes, loop wedged
up{job="ton"} == 0                                              # not answering scrapes at all
                                                                # (job = your scrape_config name)
```

Alert on both. The first arm covers a node whose collection stalled while its HTTP endpoint still
serves; it goes silent once the stale series ages out of Prometheus's ~5 min lookback, which is
exactly when the second arm takes over. It does **not** cover a gather that fails outright: the
timestamp is written before the gather runs, so a node failing every collection looks perfectly
fresh — there the HTTP 500 makes the scrape itself fail, and the second arm is the only one that
fires. Don't fold the second arm into
`absent(ton_exporter_last_collection_timestamp_seconds)`: `absent()` is evaluated over the whole
vector and yields nothing while *any* instance still reports, so with more than one target it never
fires — and Prometheus has no per-instance form of it. `up` is the per-target series Prometheus
writes itself, so it keeps the `instance` label and stays present, at 0, for a target that is down.

---

## Known gaps


- QUIC's per-connection counters (`bytes`, `packets`, `stream_bytes`, the loss and in-flight series,
  `sids_*`, `mean_rtt`) are pooled over all open connections, so they cannot be split by who dialled
  or by peer trust. Their `direction` label is the direction of the data, not of the dial. Wire and
  pre-auth transport counters likewise cannot carry trust because the peer may not be authenticated.
Worth knowing before building dashboards or alerts on these.

**Permanently-zero series.** These are emitted on every scrape but nothing increments them:
`ton_quic_app_dropped_total{direction="in"}` and `{direction="out",reason="invalid"}`;
`ton_adnl_transport_dropped_total{direction="in",reason="limited"}`;
`ton_quic_transport_dropped_total{direction="out",reason="invalid"|"limited"}`;
`ton_adnl_wire_dropped_total{direction="out",reason="invalid"|"limited"}`;
`ton_quic_wire_dropped_total{direction="in",reason="internal"}` and
`{direction="out",reason="invalid"}`;
`ton_rldp2_transport_dropped_total{direction="out"}` and `{reason="internal"}`;
four of the six cells of `ton_rldp2_app_dropped_total` and four of the six of
`ton_adnl_app_dropped_total` (both only ever use `in,limited` and `out,limited`);
and six of the 56 block-stats series — `candidate_stored` is unreachable in both families
(four series), and `ton_received_total{source="unknown"}` cannot fire (two series;
`ton_first_received_total`'s `unknown` cell *can*). The three block-broadcast sources have live
callers and are not permanently zero.

**The `tl` label sees through routing envelopes, with two deliberate limits.** The bucket unwraps
`overlay.query`/`overlay.message` (and their `WithExtra` variants), `tonNode.query`,
`overlay.unicast`, `overlay.broadcast`, `overlay.broadcastPlumtreeSimple` and
`overlay.broadcastTwostepSimple` before resolving — up to 4 nesting levels — and consults
`Object::nameof` and `Function::nameof` in **both** the `ton_api` and `lite_api` schemas.
Liteserver queries are peeled the same way `lite-client`'s `get_query_info()` does it —
`liteServer.query`'s `data` field, a bare `liteServer.queryPrefix`, and the
`liteServer.waitMasterchainSeqno` prefix — so a lite query is labelled by its method
(`liteServer.getAccountState`) rather than by its envelope. Without the `lite_api` half every
liteserver query lands in `unknown`, which on a public node is most of the query load. A malformed or truncated envelope falls back to the
envelope's own label — never to `unknown`, and never to whatever bytes happen to follow the field
it failed to walk. The limits: `dht.query` is kept as a coarse label (its `dht.node` header is not
walked), and `overlay.broadcastFec`/`overlay.broadcastFecShort` parts stay labeled as FEC parts —
the content magic physically doesn't exist per packet. Reassembled FEC broadcast content is covered
by `ton_overlay_broadcast_*` instead.

**ADNL decrypt counters exclude channel traffic.** Channel-decrypted packets go straight to the peer
pair without passing through `receive_decrypted_packet`, so `ton_adnl_transport_decrypt_packets_total`
counts only the non-channel path. Because steady-state ADNL runs over channels, this under-reports
substantially and is not comparable against `inbound_packets`.

**ADNL local-id drops are invisible.** The inbound rate limiter, keyring decrypt failures, and the
per-IP unique-peer cap all reject packets without touching any Prometheus counter — they appear only
in the legacy TL stats. These are exactly the `limited`/`invalid` events an operator wants, and they
leave an unexplained hole between `inbound_packets` and `decrypt_packets`.

**QUIC closes have no reason breakdown.** A closed connection is visible only as a decrement of
`connections_current`, with no way to distinguish idle timeout from protocol error. Handshake
outcomes are broken out (`ton_quic_transport_handshakes_total`), but the reason behind a rejection
is only the one it carries on `ton_quic_transport_dropped_total{direction="in"}` — the generic
three-value `reason` axis, shared with every other inbound drop. The `ConnectionCloseReason` and
`HandshakeFailureReason` label domains exist but nothing uses them.

**QUIC `in,invalid` can be short by one on interleaved rejects.** A single `read_pkt` call drains
coalesced and buffered packets, so ngtcp2 may discard one packet and then return an error about
another. The discard is detected, the reject folds in with it, and the two count as one. ngtcp2
exposes no per-packet attribution to separate them; counting every rejected call unconditionally
would double-count the far more common case already reflected in `pkt_discarded`.

**Inbound QUIC app-level rejects are unmetered.** A stream closed because it exceeded the per-stream
`max_size` or ran past its timeout is reported to the caller as an error, but nothing bumps
`ton_quic_app_dropped_total` — hence its permanently-zero inbound cells above.

**No internal scrape deadline.** Nothing bounds a gather: a collector that never answers leaves the
coroutine suspended, waiting scrapers hang on a body that never arrives, and every later scrape joins
the same stuck flight. Only a scraper's own client timeout ends it. A collector that *fails* is
handled — the waiting scrapers get an HTTP 500, so the scrape fails visibly — but a wedged one is
not.

**Most of the ADNL wire tier is blind on non-POSIX.** `td::UdpServer` fills its traffic counters only
under `TD_PORT_POSIX`, so on Windows `ton_adnl_wire_{bytes,packets,dropped}_total` stay zero while the
node carries traffic. The syscall and listening-socket counters still report. The transport and app
tiers are unaffected.

**Egress `is_sent` means "queued" on non-POSIX.** The Windows path hands the datagram to an
overlapped `WSASendMsg` and reports it as sent immediately, so `ton_quic_wire_{bytes,packets}_total`
count datagrams a later asynchronous completion may still drop. Accepted: the confirmation arrives on
another thread, long after the counter site. On POSIX the count reflects an accepted `sendmsg`.

**A QUIC server that fails a scrape goes stale, not backwards.** `QuicSender::collect` folds that
server's last successful answer instead of dropping it from the totals, so `ton_quic_*` counters
stay monotonic across a failed request; the cost is that the affected server's numbers freeze for
that scrape rather than being missing.

**A third workchain would abort the node.** The `workchain` label is a closed `{0, -1}` domain whose
index lookup ends in `UNREACHABLE()`. Not currently reachable from the network, but it is a hard
abort the day a third workchain exists.

**Actor latencies have no percentiles.** The actor tier exposes recent two-bucket maxima and
cumulative totals, no histograms, so there is no p50/p95 of message execution or scheduling delay —
only "the worst one recently" and "the mean, via converted
`rate(busy_ticks)/rate(messages)`". Adding histograms would put a bucket search on every actor
message in `ActorExecutor`, which is the hottest loop in the process; deliberately not paid.

**`ton_actor_scheduler_local_queue_length` sees only the per-worker queues.** Each cpu worker's
work-stealing `LocalQueue` reports its size; the scheduler's shared cpu queue (`MpmcQueue`) and its
io queue (`MpscPollableQueue`) expose no `size()`, so anything waiting in either is invisible. Work
an actor generates while running on a cpu worker goes to that worker's local queue and spills into
the shared one only when it overflows, which is why the gauge moves at all; but everything enqueued
from the io thread, from another scheduler, or from a non-actor thread bypasses it entirely. Read it
as a lower bound on the backlog, not the backlog.

**Pending timers are not exposed.** The alarm heap (`KHeap`) is private to each `Scheduler` and may
only be touched from that scheduler's own io thread, while the exporter is an ordinary actor on a cpu
worker. So there is no "actors waiting on a timeout" gauge, and a node whose work is all in the
future looks idle.

**The two worker-liveness gauges are point samples.** `ton_actor_scheduler_workers_active` and
`ton_actor_scheduler_current_execute_seconds` read `core::Debug`, which is written only while
`need_debug()` is on (as with the per-type tier), and are sampled once per scrape under a mutex.
Executions shorter than the scrape interval are simply never seen — these two catch a stall, not a
duty cycle. The exporter excludes its own collecting worker so a scrape does not manufacture an
active-worker baseline. Use converted `ton_actor_worker_busy_ticks_total` for the duty cycle. Its
snapshot includes elapsed time in the current dispatch, so a wedged scope keeps the counter rising
before it returns. The exporter worker is excluded only from the liveness point sample; its dispatch
remains ordinary scheduler occupancy in the busy counter.

**The per-type tier has no per-scheduler split inside one group**, as described in *Actors* above: a
class that runs on two schedulers in the exporter's group reports one merged set of numbers. Separate
scheduler groups are isolated.

## Legacy ADNL stats

ADNL also carries an older, unrelated stats path exposed over TL (`adnl_stats`, via
`validator-console`), fed by `add_packet_stats` and friends into windowed per-peer-pair counters. It
shares no state with the Prometheus surface above and never reaches `/metrics`.
