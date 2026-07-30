# TON node metrics

Every Prometheus metric a `validator-engine` exposes, as of branch `more-metrics-2`.

## Enabling the endpoint

Command-line only — there is no config-file field:

```
validator-engine --exporter-address <host:port>
```

Scrape `GET /metrics`. Any other path returns 404, any other method 405. There is no
authentication and no TLS, so bind it to a private interface.

The response is `application/openmetrics-text; version=1.0.0`, chunked, terminated by `# EOF`.
Each family carries a `# TYPE` line. `# HELP` is never emitted.

## How names are built

The exporter seeds a root name segment (`ton` by default, `PrometheusExporter::create()`), and
each nesting level appends a segment joined with `_`:

- `Context::with_name(seg)` and `Context::collect(node, "seg")` push a segment; `collect(node)`
  with no name adds none, letting the inner node supply the final segment.
- `Counter` renders as `<segments>_total`.
- `Gauge<T>` renders as `<segments>`, except `std::chrono` types which append `_seconds`.
- `Labeled<Inner, L...>` adds one label per axis. **Every cell is emitted on every scrape**, including
  zero-valued ones, so all label combinations below are always present in the exposition.

Registered collectors, in order: the exporter itself, `AdnlNetworkManager`, `Adnl`, `QuicSender`,
`Rldp` (rldp2), `ValidatorManagerInterface`.

## Scrape semantics

Collection is asynchronous and **sequential**: `gather()` awaits `td::actor::ask(...)` on each
collector one at a time, so a scrape costs one round-trip per collector and the subsystems are
sampled at slightly different instants. The exposition is therefore not a consistent point-in-time
snapshot across subsystems.

Values are cumulative snapshots; a scrape never resets them. Two subsystems (ADNL peer pairs,
RLDP2 connections) accumulate counters on their own actor threads and merge deltas into a
process-wide aggregate during the scrape — see the notes in those sections.

---

## Exporter

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_exporter_collectors` | gauge | — | Registered collector callbacks (6 in a full validator-engine). |
| `ton_exporter_collections_total` | counter | — | Scrapes accepted. |
| `ton_exporter_last_collection_duration_seconds` | gauge | — | Duration of the **previous** scrape (it is set after the current one is already serialized). |
| `ton_exporter_last_collection_timestamp_seconds` | gauge | — | Unix time at which the current scrape started. |

## HTTP server

Only the exporter's own server is registered, hence the constant `server="exporter"` label.

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_http_server_connections_active` | gauge | `server` | Currently open TCP connections. |
| `ton_http_server_connections_total` | counter | `server` | Accepted TCP connections. |
| `ton_http_server_requests_total` | counter | `server` | HTTP requests received, any path or method. |
| `ton_http_server_responses_total` | counter | `server`, `code` | Responses by status code. For this server: `200`, `404`, `405`, and `-1` when the response promise failed. |

---

## ADNL

Two collectors: `AdnlNetworkManager` supplies the wire tier, `Adnl` (the peer table) the transport
and app tiers.

### Wire

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_adnl_wire_bytes_total` | counter | `direction=in\|out` | UDP payload bytes at the socket. |
| `ton_adnl_wire_packets_total` | counter | `direction` | Datagrams at the socket. |
| `ton_adnl_wire_syscalls_total` | counter | `direction` | recvmsg/sendmmsg calls at the socket, read from `td::UdpServer`'s counters and folded in as deltas during the scrape. |
| `ton_adnl_wire_dropped_total` | counter | `direction` | Inbound: kernel receive-queue overflow (`SO_RXQ_OVFL`, folded in as a delta during the scrape) plus datagrams the network manager refused: no callback installed, socket error, packet under 32 bytes, no `InDesc` for the port. Outbound: unknown source id or no matching out rule. No `reason` axis at this tier. |
| `ton_adnl_wire_listening_sockets` | gauge | — | Bound UDP sockets. |

### Transport

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_adnl_transport_inbound_packets_total` | counter | — | Packets entering the peer table, counted before any routing decision. |
| `ton_adnl_transport_decrypt_packets_total` | counter | — | Packets decrypted **by a local id** and parsed. Channel-decrypted packets bypass this — see Known gaps. |
| `ton_adnl_transport_decrypt_bytes_total` | counter | — | On-wire ciphertext size of those same packets. |
| `ton_adnl_transport_dropped_total` | counter | `direction`, `reason=invalid\|limited\|internal` | Peer-table and peer-pair drops. `in,invalid` dominates: short packets, category mismatch, unknown destination, reinit-date and seqno checks, bad signature, huge-message reassembly failures. `out,limited` is queue expiry and the 10 MiB queue cap. `in,limited` is never incremented. |
| `ton_adnl_transport_local_ids` | gauge | — | Local ADNL ids. |
| `ton_adnl_transport_peers` | gauge | — | Distinct remote nodes. |
| `ton_adnl_transport_peer_pairs` | gauge | — | `(local_id, peer_id)` pairs. |
| `ton_adnl_transport_channels` | gauge | — | Registered ADNL channels. |
| `ton_adnl_transport_static_nodes` | gauge | — | Static nodes from config. |

### App

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_adnl_app_bytes_total` | counter | `kind=message\|query\|answer`, `direction`, `tl` | Application payload bytes, bucketed by the payload's leading TL constructor magic. Outbound is accounted at the single send choke point, before the huge-message split. |
| `ton_adnl_app_messages_total` | counter | same | Message count for the same events. |
| `ton_adnl_app_dropped_total` | counter | `direction`, `reason` | Payloads refused at the app boundary for size (the 8 KiB message cap, the 1024 B answer MTU). Only `limited` is ever used. |

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
| `ton_quic_wire_syscalls_total` | counter | `direction` | recvmmsg calls returning at least one message; every sendmmsg call. |
| `ton_quic_wire_dropped_total` | counter | `direction` | Inbound: kernel receive-queue overflow (read as a delta from `SO_RXQ_OVFL` **during the scrape**) plus per-message socket errors. Outbound: stateless datagrams the socket refused, which are not retried. Normal connection egress is retried and never counted here. |
| `ton_quic_wire_listening_sockets` | gauge | — | Distinct bound UDP ports. |

### Transport

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_quic_transport_connections_total` | counter | — | Connections ever installed, including ones that never completed the handshake. |
| `ton_quic_transport_connections_current` | gauge | — | Currently open connections. |
| `ton_quic_transport_bytes_total` | counter | `direction` | ngtcp2 packet bytes. |
| `ton_quic_transport_packets_total` | counter | `direction` | ngtcp2 packet count. |
| `ton_quic_transport_stream_bytes_total` | counter | `direction` | STREAM payload. Inbound at delivery; **outbound at ACK time**, so it trails the app tier by everything in flight or lost. |
| `ton_quic_transport_bytes_lost_total` | counter | — | Bytes in packets declared lost by loss detection. |
| `ton_quic_transport_packets_lost_total` | counter | — | Packets declared lost. |
| `ton_quic_transport_bytes_in_flight` | gauge | — | ngtcp2 bytes in flight. |
| `ton_quic_transport_bytes_unacked` | gauge | — | Stream bytes handed to ngtcp2, not yet acked. |
| `ton_quic_transport_bytes_unsent` | gauge | — | App-buffered stream bytes not yet handed to ngtcp2. |
| `ton_quic_transport_sids_total` | counter | — | **Peer-initiated** bidi streams accepted. Locally opened streams are not counted. |
| `ton_quic_transport_sids_current` | gauge | — | Open streams, counting both directions of initiation. |
| `ton_quic_transport_mean_rtt_seconds` | gauge | — | Connection-weighted mean smoothed RTT over open connections. |
| `ton_quic_transport_dropped_total` | counter | `direction`, `reason` | `in,invalid`: unroutable datagram, rejected handshake, protocol violation, plus ngtcp2's own discarded-packet delta. `in,limited`: per-IP flood limiter. `in,internal`: connection creation failure. `out,internal`: egress production failure. `out,invalid` and `out,limited` are never incremented. |

### App

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_quic_app_bytes_total` | counter | `kind`, `direction`, `tl` | Inner ADNL payload bytes carried over QUIC streams, measured outside the `quic_message`/`quic_query`/`quic_answer` wrapper. |
| `ton_quic_app_messages_total` | counter | same | Message count. |
| `ton_quic_app_dropped_total` | counter | `direction`, `reason` | **Always 0** — no QUIC code path calls it. See Known gaps. |

---

## RLDP2

One collector on `Rldp`. Per-connection counters are drained into an aggregate the same way ADNL
drains peer pairs; a connection dropped after the 120 s idle timeout drains from `tear_down`, so
counts survive connection churn.

### Wire

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_rldp2_wire_adnl_bytes_total` | counter | `direction` | Bytes of RLDP2 datagrams exchanged with ADNL. |
| `ton_rldp2_wire_adnl_messages_total` | counter | `direction` | Count of those datagrams — "messages" here means datagrams, not app messages. |

### Transport

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_rldp2_transport_transfers_total` | counter | `direction`, `state=completed\|failed\|timeout` | Terminal outcome of a whole transfer (a reassembled message, not a packet); errors carrying `ErrorCode::timeout` are classified as `timeout`, everything else as `failed`. |
| `ton_rldp2_transport_connections` | gauge | — | Live connections. |
| `ton_rldp2_transport_queries_pending` | gauge | — | Outbound queries awaiting an answer. |
| `ton_rldp2_transport_dropped_total` | counter | `direction`, `reason` | Protocol-layer rejects: malformed TL, bad FEC type or symbol size, bad seqno, part index or size mismatch (`invalid`); transfer over the size cap (`limited`). All are inbound; `out` and `internal` are always 0. |

### App

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_rldp2_app_bytes_total` | counter | `kind`, `direction`, `tl` | Payload bytes crossing the RLDP↔app boundary. |
| `ton_rldp2_app_messages_total` | counter | same | Message count. |
| `ton_rldp2_app_dropped_total` | counter | `direction`, `reason` | `in,limited`: datagram from a peer with no permitted connection, or an inbound answer exceeding our query's `max_answer_size`. `out,limited`: our answer exceeded the requester's `max_answer_size`. |

---

## Overlay

One collector on `Overlays` (the overlay manager). Records broadcast **content** — the naked TL
payload, after FEC reassembly — where the transport tiers can only see wire bytes and FEC parts.
Outbound is counted on the manager at the send choke points; inbound accumulates per-overlay actor
and is drained into the manager's aggregate on scrape (the same drain/absorb idiom as ADNL peer
pairs), with a `tear_down` flush so a dying overlay's counts survive.

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_overlay_broadcast_bytes_total` | counter | `direction=in\|out`, `tl` | Broadcast content bytes. `out` at the four terminal `send_broadcast*` entry points, pre-FEC-encoding. `in` at `deliver_broadcast`, post-reassembly. |
| `ton_overlay_broadcast_messages_total` | counter | same | Broadcast count for the same events. |

Two semantics worth knowing. Sizes here are content bytes while the transport tiers count wire
bytes, so `*_app_*{tl="overlay.broadcastFec"}` minus this tier's inbound bytes prices the FEC
redundancy and duplicate reception. And `direction="in"` includes self-originated broadcasts — a
node delivers its own broadcast to its own callbacks, so a locally originated broadcast increments
both directions; `in` means "content this overlay delivered", not "received from peers".

## Blocks

From `ValidatorManagerImpl`. Note these two sit directly under the root prefix with no subsystem
segment, unlike every other family.

| metric | type | labels | meaning |
|---|---|---|---|
| `ton_first_received_total` | counter | `workchain=0\|-1`, `source` | For each block that reaches "applied", one increment for the **earliest** source that delivered it. |
| `ton_received_total` | counter | `workchain`, `source` | For each applied block, one increment **per distinct source** that delivered it, including sources arriving after the block was applied. |

`source` values: `unknown`, `block_broadcast_public`, `block_broadcast_fast_sync`,
`block_broadcast_custom`, `block_download`, `candidate_broadcast_public`,
`candidate_broadcast_fast_sync`, `candidate_broadcast_consensus`, `candidate_broadcast_custom`,
`candidate_finality_public`, `candidate_finality_fast_sync`, `candidate_finality_custom`,
`candidate_stored`, `block_accepted`.

Cardinality is fixed at 2 × 14 = 28 series per family; `workchain` is a closed two-value domain,
not a dynamic label.

---

## The data ladder

`metrics/well-known.h` documents an intended tier relationship: wire bytes, minus kernel-queue
drops, minus transport drops, gives useful bytes; minus protocol overhead gives stream bytes; minus
ADNL wrapping gives app bytes. Only the top and bottom rungs are actually implemented — nothing
emits the `useful` or `stateless_dropped` middle rungs today.

What you can actually relate, approximately:

```
ton_adnl_wire_packets_total{direction="in"}
  − ton_adnl_wire_dropped_total{direction="in"}            too short / no InDesc / no callback
≈ ton_adnl_transport_inbound_packets_total
  − ton_adnl_transport_dropped_total{direction="in"}       routing and packet-check rejects
  − (unmetered: rate limiter, decrypt failures, per-IP peer cap)
≈ ton_adnl_transport_decrypt_packets_total                 non-channel packets only
  − ADNL framing overhead
≈ ton_adnl_app_bytes_total
```

The `≈` are real. Tiers are sampled at different instants within one scrape, counts are taken at
different points in a packet's life, and several drop paths are unmetered.

---

## Known gaps

Worth knowing before building dashboards or alerts on these.

**Permanently-zero series.** These are emitted on every scrape but nothing increments them:
`ton_quic_app_dropped_total` (all six);
`ton_adnl_transport_dropped_total{direction="in",reason="limited"}`;
`ton_quic_transport_dropped_total{direction="out",reason="invalid"|"limited"}`;
`ton_rldp2_transport_dropped_total{direction="out"}` and `{reason="internal"}`;
three of six cells of `ton_rldp2_app_dropped_total`; and eight of the 56 block-stats series, whose
`source` values are unreachable in current code.

**The `tl` label sees through routing envelopes, with two deliberate limits.** The bucket unwraps
`overlay.query`/`overlay.message` (and their `WithExtra` variants), `tonNode.query`,
`overlay.unicast` and `overlay.broadcast` before resolving, and consults both `Object::nameof` and
`Function::nameof`; a malformed envelope falls back to the envelope's own label, never to
`unknown` (design and history in `tl-magic-research.md`). The limits: `dht.query` is kept as a
coarse label (its `dht.node` header is not walked), and `overlay.broadcastFec` parts stay labeled
as FEC parts — the content magic physically doesn't exist per packet. Reassembled FEC broadcast
content is covered by `ton_overlay_broadcast_*` instead.

**ADNL decrypt counters exclude channel traffic.** Channel-decrypted packets go straight to the peer
pair without passing through `receive_decrypted_packet`, so `ton_adnl_transport_decrypt_packets_total`
counts only the non-channel path. Because steady-state ADNL runs over channels, this under-reports
substantially and is not comparable against `inbound_packets`.

**ADNL local-id drops are invisible.** The inbound rate limiter, keyring decrypt failures, and the
per-IP unique-peer cap all reject packets without touching any Prometheus counter — they appear only
in the legacy TL stats. These are exactly the `limited`/`invalid` events an operator wants, and they
leave an unexplained hole between `inbound_packets` and `decrypt_packets`.

**QUIC has no connection-close or handshake-failure metric.** A closed connection is visible only as
a decrement of `connections_current`, with no way to distinguish idle timeout from protocol error.
The `ConnectionCloseReason` and `HandshakeFailureReason` label domains exist but nothing uses them.

**QUIC totals can go backwards.** `QuicSender::collect` skips any server whose scrape request
failed, so a single failed request makes every `ton_quic_*` total drop for that scrape, which
Prometheus reads as a counter reset.

**A third workchain would abort the node.** The `workchain` label is a closed `{0, -1}` domain whose
index lookup ends in `UNREACHABLE()`. Not currently reachable from the network, but it is a hard
abort the day a third workchain exists.

## Legacy ADNL stats

ADNL also carries an older, unrelated stats path exposed over TL (`adnl_stats`, via
`validator-console`), fed by `add_packet_stats` and friends into windowed per-peer-pair counters. It
shares no state with the Prometheus surface above and never reaches `/metrics`.
