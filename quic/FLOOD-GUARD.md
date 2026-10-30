# QUIC resource limits

`FloodGuard` owns admission, stream lifetime, memory accounting, ingress credit shaping, and
connection eviction for one `QuicServer` listening port. Its `Host` interface applies decisions to
the transport. All guard operations run on the server actor, without additional locks.

The limits protect legitimate peers from resource exhaustion by other peers. They are not an
absolute process-RSS bound or protection against a UDP packet flood. Transport reordering, TLS,
kernel buffers, and work queued beyond ADNL delivery have separate lifetimes and limits.

## Peer classes

Trust is a local resource classification, not authorization. Permanent overlay registrations mark
peers trusted, as are operator-configured custom-overlay nodes. Plumtree eager registrations raise
the allowed message size without granting trust, because eager membership can be influenced by
remote traffic. MTU registrations aggregate by maximum size; trust aggregates independently with
logical OR.

Handshake authentication binds the connection to its peer identity before classification. Registry
updates reclassify existing connections and move their reassembly and stream-metadata charges between
trusted and untrusted pools. Already-dispatched work stays in its admission-time pool until
completion, and already-open streams keep the deadline and size cap they were opened with.
Inbound trusted peers have separate, finite budgets. Outbound connections carry our own requests
and use caller deadlines and answer-size limits instead of inbound admission. They advertise zero
peer-initiated bidirectional and unidirectional stream credit: replies use the bidirectional
streams we opened, while message confirmations are transport acknowledgements.

## Admission and scheduling

Tokenless Initial packets receive stateless Retry, subject only to the global admission gate.
Per-IP budgets and discourage throttles are consumed only after the Retry token proves the source
address; with `stateless_retry` disabled they run on an unproven address. Existing connection IDs
route before new-connection admission.

Half-open connections have a deadline and per-IP/global caps. Under the global cap, the oldest
half-open connection is evicted to leave new handshakes a chance to complete. Classified untrusted
connections additionally have a count cap and an application-inactivity timeout.

Untrusted inbound traffic shares a work-conserving ingress bucket whose burst is one second of its
rate. Shaping starts at connection creation and credit is granted only by the paced round-robin
drain. The bucket controls returned connection-level `MAX_DATA` credit; it does not prevent
receiving, decrypting, parsing, or ACKing packets already permitted by the advertised windows.
Egress schedules trusted/untrusted classes with a 4:1 preference and caps buffered replies per
connection.

## Memory lifetimes

Two separate pools are needed because buffered streams can be reclaimed, while already-dispatched
handlers cannot be cancelled merely by closing the connection.

| Lifetime | Accounted resources | Release point |
|---|---|---|
| Open stream | Fixed `stream_state_overhead_bytes` reservation | Stream state removal or connection teardown |
| Reassembly | Retained application buffer storage | Dispatch or stream failure |
| Dispatched request | Retained request storage plus `inflight_request_overhead_bytes` | Actual query completion or ADNL message delivery acknowledgement |

Peer stream opening reserves metadata and arms a pending deadline before contiguous payload arrives.
This includes a stream whose first frame leaves a gap at offset zero. Fragmented input is coalesced
so a sender cannot allocate a retained fragment object for every one-byte callback. The common
single-chunk path avoids an unnecessary copy. Counters charge retained backing capacity, including
unused growth space; a growth copy temporarily holds both the old and new allocation.

Dispatch checks the in-flight budget before delivering the request. If the peer closes its stream
or connection while the handler is still running, its charge moves into the orphaned-work table.
Both payload storage and processing overhead remain charged until the handler finishes. Therefore
recycling stream credit does not make tiny, outstanding requests free.

Untrusted reassembly pressure evicts the fattest connections above the 90% high watermark until
usage falls below the 75% low watermark. Trusted reassembly pressure refuses further growth
(whichever stream's growth no longer fits fails) without evicting validator connections, and logs a
warning once the pool passes 75%. In-flight pressure refuses new dispatches in the relevant trust
class.

## Churn and reserved-only mode

An unclean peer close before dispatch consumes churn credit (64 tokens per connection, refilling
64/s), including a reset before the first data callback. Clean empty streams, local streams, and
guard-imposed deadlines are excluded. A stream-open refused because the peer exhausted its own
connection budget is charged as churn; one refused because the shared pool is full is not. A stream
failed for per-connection overflow while receiving data is reset without a churn charge.

Guard closes carry a reason: 429 for churn, 503 for eviction, 403 for reserved-only shedding, and
200 for ordinary inactivity, which does not discourage. After a 429 the IP is discouraged for 30
seconds, after a 403 for 300 seconds: it may open one connection per 5 seconds. That is a throttle,
not an IP ban, so a trusted validator sharing a NAT with an abusive peer still has opportunities to
authenticate. A 503 is a 3-second refusal of the IP, so the freed slot is not instantly retaken.
The peer does not act on these close codes yet; that is client-side follow-up work.

Reserved-only mode is API-only (`set_reserved_only`, no command-line flag). It allows half-open
connections to finish identity classification, then rejects untrusted inbound peers. It also sheds
existing untrusted connections and live trust downgrades. Deferred 403 closes recheck both the
current mode and current trust: promotion or disabling the mode cancels a stale decision. Deferred
429 closes recheck trust: promotion cancels them.

## Defaults and configuration

`FloodLimits` is the source of truth. These defaults apply per listening port, except the dial
backoff and the bare `send_query` answer cap, which apply per `QuicSender`:

| Resource | Default |
|---|---|
| Half-open connections | 128 per IP; 16,384 total (a few hundred MB of transient ngtcp2/TLS state); 5-second deadline, tighter than the 15-second idle timeout |
| New connections per IP | 10 per 0.2 seconds |
| Concurrent connections per IP | 1,000 |
| Classified untrusted inbound connections | 4,096 |
| Untrusted ingress credit | 50 MiB/s (400 Mbit/s) shared bucket |
| Accounted reassembly per connection | 256 MiB |
| Untrusted/trusted reassembly pools | 1 GiB / 4 GiB |
| Untrusted/trusted in-flight pools | 1 GiB / 4 GiB |
| Stream metadata reservation | 16 KiB per live stream (about 65k untrusted stream records in the 1 GiB pool) |
| Processing reservation | 16 KiB per dispatched request |
| Untrusted/trusted pending-stream deadlines | 10 / 300 seconds (a broadcast block arrives in under 200 ms) |
| Discourage after 429 / 503 / 403 | 30 / 3 / 300 seconds; one connection per 5 seconds while throttled |
| Stream churn | 64 abandonments per connection, refilling 64/s |
| Untrusted application-inactivity timeout | 120 seconds |
| Untrusted/trusted buffered reply caps | 256 MiB / 1 GiB per connection |
| Bare `send_query` maximum answer | 16 MiB; `send_query_ex` supplies an explicit limit |
| Failed outbound dial backoff | 1 second, doubling to 60 seconds |

`--quic-flood-control` configures the per-IP connection limit; `0` allows one connection per IP.
`-1` disables that limit and both new-connection rate limiters, but does not disable the separate
half-open caps or discourage table.
Other limits are currently programmatic; effective settings are logged at startup. Production
values still need validation against legitimate validator and full-node load.

## Remaining boundaries

- Transport reorder storage is outside the application-buffer counters. QUIC receive windows and
  the stream-opening/deadline limits constrain it, but the aggregate guard byte pool does not meter
  ngtcp2 allocations. The default connection receive window starts at 4 MiB and can grow to 24 MiB;
  these protocol windows are not interchangeable with a memory/RSS limit. Exact transport memory
  accounting needs a separate transport-layer change.
- A message's charge lasts through ADNL delivery acknowledgement. The default subscriber callback
  may only enqueue work into another actor and acknowledge immediately. The guard does not bound
  that downstream mailbox. End-to-end acknowledgement/accounting remains separate work.
- Trusted ingress is unshaped. Egress has buffer limits and class preference, but no aggregate
  bandwidth cap. Handshake CPU still depends on admission settings and external packet filtering.
- The `MAX_DATA` credit ngtcp2 re-grants by itself for an unfinished peer stream we stop or the peer
  resets is charged to the bucket when that happens, bounded by the stream's receive window
  (`max_stream_window`), so the shaper governs all credit a shaped peer receives. Packets that
  physically arrive are still decrypted and parsed, as for any UDP packet.
- RFC 9221 datagrams (off by default) bypass the guard entirely.
- Answers to our own queries are bounded by the 256 MiB per-connection reassembly budget and the
  caller's answer limit; production answers are at most 16 MiB.

Flood pressure and decision metrics are documented in [METRICS.md](../metrics/METRICS.md#flood-protection).
