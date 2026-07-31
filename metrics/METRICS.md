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
  `reason`, `result`, `state`, `workchain`, `source`. (An `outcome` axis is defined too, but its only holder
  `TransferStats` is never instantiated, so no family carries it.) **Every cell of a closed axis is
  emitted on every scrape**, including zero-valued ones, so all such label combinations below are
  always present in the exposition (which is why the permanently-zero series in Known gaps still
  show up).
  The three **open** label axes behave differently and emit only values actually observed: `code` on
  the HTTP responses family, `tl` on the traffic and latency buckets, and `op` on the perf families. The `tl` buckets always
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
| `ton_perf_op_ticks_total` | counter | `op` | CPU ticks (`rdtsc`) spent in those executions. Absolute values are machine-specific, but `rate(ton_perf_op_ticks_total) / rate(ton_perf_ops_total)` is a usable average cost per operation, and its trend catches regressions. |

## HTTP server

Only the exporter's own server is registered, hence the constant `server="exporter"` label.

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_http_server_connections_active` | gauge | `server` | Currently open TCP connections. |
| `ton_http_server_connections_total` | counter | `server` | Accepted TCP connections. |
| `ton_http_server_requests_total` | counter | `server` | HTTP requests received, any path or method. |
| `ton_http_server_responses_total` | counter | `server`, `code` | Responses by status code. For this server: `200`, `404`, `405`, `500` when the gather behind a `/metrics` scrape failed, and `-1` when the response promise failed. |

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
| `ton_adnl_query_failed_total` | counter | `tl` | Of those queries, the ones that answered with an error (or were abandoned). |

A query slower than 1 s also gets an `INFO` log line with its `tl` name, the other end's id under
`peer=` (the source here, the destination for the outbound families below) and the elapsed time — no
payload size. The throttle is per bucket, one line per 10 s per site, since losing a peer makes every
one of its queries slow at once. These logs are a temporary diagnostic, to be removed once the
latency histograms are trusted.

### Outbound: roundtrips and deliveries

The outbound mirror is **per transport**, measured where the transport accepts the send:

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_adnl_query_roundtrip_seconds` / `ton_rldp2_…` / `ton_quic_…` | histogram | `tl`, `le` | Transport-accept to answer for queries we send: network + peer processing + transfer time. Errors and timeouts land in the matching `…_query_roundtrip_failed_total{tl}`. |
| `ton_rldp2_message_delivery_seconds` / `ton_quic_…` | histogram | `tl`, `le` | Transport-accept to the protocol's receipt confirmation for fire-and-forget messages: rldp2 confirms via the transfer's completion (`on_sent`), QUIC via the empty response the receiver answers every message with. Failures (including a connection closing with messages still in flight) land in `…_message_delivery_failed_total{tl}`. |

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
| `ton_quic_app_bytes_total` | counter | `kind`, `direction`, `tl` | Inner ADNL payload bytes carried over QUIC streams, measured outside the `quic_message`/`quic_query`/`quic_answer` wrapper. |
| `ton_quic_app_messages_total` | counter | same | Message count. |
| `ton_quic_app_dropped_total` | counter | `direction`, `reason` | Fire-and-forget message sends that failed: `out,limited` when the peer's stream-count credit blocked opening a stream (`NGTCP2_ERR_STREAM_ID_BLOCKED`), `out,internal` for any other send failure. Query failures are not counted here — they propagate to the caller. Inbound cells are never incremented. |

### Outbound latency

Described in full under ADNL → *Outbound: roundtrips and deliveries*; the QUIC families are:

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_quic_query_roundtrip_seconds` | histogram | `tl`, `le` | Send-accept to answer for queries we send over QUIC. Connection setup is deliberately outside the measured window. |
| `ton_quic_query_roundtrip_failed_total` | counter | `tl` | Of those, the ones that errored or timed out. |
| `ton_quic_message_delivery_seconds` | histogram | `tl`, `le` | Send-accept to the empty response the receiver answers every fire-and-forget message with. |
| `ton_quic_message_delivery_failed_total` | counter | `tl` | Of those, the ones that never got their confirmation (including a connection closing with messages in flight). |

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

## Blocks

From `ValidatorManagerImpl`. Note these two sit directly under the root prefix with no subsystem
segment, unlike every other family.

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

**What am I receiving, by type?** Query one transport at a time:

```promql
topk(10, sum by (tl) (rate(ton_quic_app_messages_total{kind="query",direction="in"}[5m])))
```

Swap `messages`→`bytes` for traffic share; to fold in response volume (answers usually dominate
bytes) widen to `kind=~"query|answer"` — `kind` is an equality matcher, so a second `kind="answer"`
would match nothing rather than add a case.

**Do not reach for `{__name__=~"ton_(adnl|quic|rldp2)_app_bytes_total"}` to do all three at once.**
`rate()` drops `__name__`, and the app tier carries the *same* label set on every transport, so the
three series collapse into one another and Prometheus fails the whole query with `vector cannot
contain metrics with the same labelset`. Nothing after `rate()` can repair it — `label_replace`
runs too late. If you want a cross-transport total, synthesize the distinguishing label in a
recording rule, where each `rate()` is evaluated separately:

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
sum by (tl) (rate(ton_quic_message_delivery_failed_total[5m]))
  / sum by (tl) (rate(ton_quic_message_delivery_seconds_count[5m]))
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
  the way `connections_*` and `handshakes` are. Their `direction` label is the direction of the data,
  not of the dial.
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
and 18 of the 56 block-stats series — `block_broadcast_public`, `block_broadcast_fast_sync`,
`block_broadcast_custom` and `candidate_stored` are unreachable in both families (16 series), and
`ton_received_total{source="unknown"}` cannot fire either (2 series; `ton_first_received_total`'s
`unknown` cell *can*).

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

## Legacy ADNL stats

ADNL also carries an older, unrelated stats path exposed over TL (`adnl_stats`, via
`validator-console`), fed by `add_packet_stats` and friends into windowed per-peer-pair counters. It
shares no state with the Prometheus surface above and never reaches `/metrics`.
