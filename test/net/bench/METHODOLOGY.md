# Transport benchmark methodology

The workload and measurement contract behind [README.md](README.md). The catalog includes both
today's runnable tests and the intended scale suite; the status column is normative:

- **maintained**: run by `suite.sh`;
- **available**: expressible with `bench.sh`, but not in the default suite;
- **planned**: benchmark contract agreed, scalable driver not implemented yet.

## Benchmark catalog

| workload | exact shape | implementations / transports | status |
| --- | --- | --- | --- |
| `idle-small` | 32 established application-idle peer sessions -> one server | all QUIC implementations; TON ADNL/RLDP2 | maintained |
| `idle-scale` | 1K, 10K and optionally 100K established application-idle sessions -> one server | quic-go and Quinn drivers available; TON multi-connection generator planned | available / planned |
| `ihave-1x20` | one sender -> 20 neighbours; one 240-byte IHAVE each, 20 sends/event | all QUIC implementations; TON ADNL/RLDP2 | maintained |
| `ihave-40x20` | one sender -> 20 neighbours; 40 IHAVEs each immediately, 800 sends/event at 6,000 sends/s | all QUIC implementations; TON ADNL/RLDP2 | maintained |
| `fanin` | several independent neighbours -> one server; default 8 x 1,000 messages/s | all QUIC implementations; TON ADNL/RLDP2 | maintained |
| `sat-1x1` | ceiling probe: one connection, offered 1M msg/s in bursts of 100 | all QUIC implementations; TON ADNL/RLDP2 | maintained |
| `sat-20x400` | ceiling probe: 20-way fan-out, offered 1M msg/s in bursts of 400 | all QUIC implementations; TON ADNL/RLDP2 | maintained |
| `bulk-common` | two clients -> one server; 7,680-byte request and 1,024-byte answer | all QUIC implementations; TON ADNL/RLDP2 | maintained |
| `bulk-large` | two clients -> one server; 64-byte request and 256 KiB answer | all QUIC implementations; TON RLDP2 | maintained |
| `mixed-scale` | 100K idle sessions plus 20 active neighbours running both IHAVE shapes | QUIC, ADNL, RLDP2 | planned |

QUIC has connections, while ADNL and RLDP2 retain different peer/session objects. Cross-transport
scale results therefore use the common term **established peer session** and also report each
transport's native gauge. For QUIC, `idle-scale=100K` specifically means 100K live,
application-ready QUIC connections; TON peer authentication and the native test certificate are
not equivalent identity schemes.

## Implemented transport suite

`bench.sh` starts separate server and client/generator processes and measures one workload.
`-I ton|quic-go|quinn` selects the implementation and `-P quic|adnl|rldp2` selects the wire
transport. Native implementations support only `-P quic`. `suite.sh` runs the maintained matrix;
its default remains TON only.

```sh
cmake -B cmake-build-relwithdebinfo -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C cmake-build-relwithdebinfo bench-rldp

test/net/bench/suite.sh
PROTOCOLS="quic adnl" DURATION=60 test/net/bench/suite.sh

(cd test/net/bench/quicbench-go && go build -o quicmsgbench-go .)
cargo build --release --manifest-path test/net/bench/quicbench-rs/Cargo.toml
IMPLEMENTATIONS="ton quic-go quinn" PROTOCOLS=quic REPETITIONS=3 test/net/bench/suite.sh
```

Never use a debug or sanitizer build for measurements. Override `BENCH` when comparing another
TON build, `GO_BENCH` or `QUINN_BENCH` for a native binary, and `THREADS` when the one-worker-per-
process default is not the topology you want. Native parity runs use TON-sized stream credits,
bounded flow-control windows, and 5s/15s keepalive/idle timers by default; set `NATIVE_TUNED=0`
only when separately measuring library defaults. Cross-library runs use CUBIC because all three
implementations expose it; set `CC=bbr` or `CC=reno` for separately labelled TON QUIC runs.
Set `FANIN_RATE` to change the suite's per-generator fan-in rate; the default is 1,000 messages/s.
Set `DENSE_RATE` to change the aggregate dense-IHAVE rate; the default 6,000 sends/s is the highest
common rate verified across every maintained transport on this host.
`suite.sh` interleaves implementations scenario by scenario. Use `REPETITIONS` for repeated rows;
three repetitions are a useful smoke comparison and five or more are expected for an A/B claim.

Examples:

```sh
# Normal IHAVE: 200 events/s, each event sends once to all 20 neighbours.
test/net/bench/bench.sh -I quic-go -T message -P quic -n 20 -s 240 -m 4000 -B 20

# Dense IHAVE: 7.5 events/s, each event sends 40 IHAVEs to all 20 neighbours.
test/net/bench/bench.sh -T message -P quic -n 20 -s 240 -m 6000 -B 800

# Multiple independent ADNL senders into one receiver.
test/net/bench/bench.sh -T message -P adnl -c 8 -s 240 -m 1000

# Near-limit raw ADNL request; its query response envelope is 1 KiB.
test/net/bench/bench.sh -T query -P adnl -c 2 -i 2 -s 7680 -r 1024

# Large reliable download; run the same shape with -P rldp2.
test/net/bench/bench.sh -T query -P quic -c 2 -i 2 -s 64 -r 262144

# Establish 64 peer sessions, then stop application traffic.
test/net/bench/bench.sh -T idle -P adnl -c 64

# One four-worker quic-go generator owning 100K connections to one server.
READY_TIMEOUT=180 THREADS=4 test/net/bench/bench.sh \
  -I quic-go -P quic -T idle -c 1 -C 100000 -w 30 -d 60
```

### IHAVE semantics

IHAVE has two independent dimensions: neighbour count and IHAVEs per neighbour. `-B` is the total
number of sends in one event, while `-m` is the aggregate message rate per client. Event frequency
is therefore `rate / burst-size`.

The current fan-out driver visits destinations round-robin. With 20 neighbours, `-B 20` sends once
to peers 0 through 19. `-B 800` repeats that round 40 times, corresponding to:

```text
IHAVE 1  -> all 20 neighbours
IHAVE 2  -> all 20 neighbours
...
IHAVE 40 -> all 20 neighbours
```

`ihave-1x20` is the small-packet regression and cross-peer scheduling case. GSO cannot combine
different UDP destinations, although `sendmmsg` may submit several descriptors in one syscall.
`ihave-40x20` additionally queues 40 messages per connection and is the production-shaped test for
per-connection packet filling, stream credit and GSO.

The driver skips missed event deadlines rather than generating a catch-up storm. These are
controlled-load efficiency tests, not saturation searches; the report shows offered-rate attainment
separately from receiver delivery.

Raw ADNL accepts request and message payloads up to 8 KiB, while its query answer envelope is 1 KiB.
It therefore participates in `bulk-common`, but not `bulk-large`.

### Measurement contract

Each client first completes a small query to every destination. `bench.sh` waits for readiness,
warms the live workload for `-w` seconds, then takes exporter, process-CPU and RSS snapshots around
a fixed `-d` second window. Handshakes, route setup, PMTU/congestion ramp-up and result formatting
are outside the measurement. The side under test is the server for fan-in/idle and the single client
for fan-out; exporters on both sides are scraped.

Each exporter group's rate uses the midpoints of its own start and end scrapes. Query throughput
counts successful client-side round trips, not requests merely observed by the server. Message
throughput counts receiver-observed delivery, not submission. The two exporter groups are not an
atomic distributed snapshot, so a boundary can differ by one whole burst; keep the measurement
long enough that one burst is well below the desired error bound. The maintained default is 20s.

The report includes:

- submitted and delivered message rates, pacing percentage and delivery percentage;
- logical IHAVEs/s for fan-out;
- application MiB/s, CPU, UDP datagrams/syscalls and wire bytes per delivered message;
- QUIC packets/nonempty flush and the share of flushes above 16 packets;
- GSO segments/descriptor, `sendmmsg` descriptors/send and combined datagrams/send;
- target and start/end peer-session gauges, CPU/RSS from both sides, and
  application/transport/socket drops.

The native drivers expose a neutral application contract:
`bench_connections_current`, sent/received message counters, sent/completed query counters,
`bench_errors_total`, and sent/received byte counters. `report.py` normalizes TON's `ton_*` metrics
to the same report fields and also uses its wire, syscall and batching metrics. Unavailable and
QUIC-only fields are written as `NA`, never zero.

A run is `INVALID` if required metric families are absent, it has drops or transport/query failures,
handles no traffic, submits less than 90% of the requested message rate, delivers less than 99% of
the submitted rate, or does not retain exactly the requested peers at both snapshots. Raw snapshots,
logs, the self-describing `manifest.tsv`, and `summary.tsv` remain in the artifact directory.
`ok` is a correctness/load-attainment verdict, not a resource-regression verdict; compare CPU,
retained RSS and RSS growth across repeated runs explicitly.

## Connection scale

`-c` is the number of generator processes. For quic-go and Quinn, `-C` is the number of connections
owned by each generator over one shared UDP endpoint; the expected connection count is `c * C`.
For fan-out, `bench.sh` sets `C` equal to the server count so that there is exactly one connection to
each destination. TON currently requires `-C 1`, so it must not be used as `-c 100000`: process
overhead would dominate before the transport was meaningfully exercised.

Native QUIC scale runs use one measured server and a small number of generators, each owning many
live connections. Every connection must complete the application-level readiness probe before
warm-up begins. Connection starts are bounded by `CONNECT_INFLIGHT` and paced by `CONNECT_RATE`
(defaults 64 and 2,000/s). Set `READY_TIMEOUT` high enough for setup; connection creation, repair of
connections lost during the readiness ramp, and teardown remain outside the measured window. The Go
driver repairs pre-readiness losses; Quinn fails the run if a connection closes during its stability
wait. For a single native idle generator, `first_pass_seconds` is the first-pass connect-and-probe
ramp, while `setup_seconds` also includes repair and the final stability gate. A comparable
multi-connection TON generator is still planned.

Admission limits need explicit treatment. The current benchmark's `--no-limits` lifts QUIC's
benchmark-facing limits, but ADNL still rejects more than 60 recently seen peer identities from one
source IP. A 100K ADNL/RLDP2 generator therefore needs either realistic source-address diversity or
a narrowly scoped benchmark-only override. Admission-limit testing remains a separate workload;
the chosen mode must be recorded with every scale result.

The `idle-scale` phases are:

1. start and pre-scrape the server, then record its CPU/RSS baseline;
2. create sessions at a controlled rate with bounded setup concurrency;
3. require a successful readiness query on every identity;
4. wait until exactly the target number is ready, then settle;
5. measure application-idle behaviour for at least 60 seconds;
6. verify the native session count at both snapshots and tear down outside the window.

“Application idle” does not disable transport work. In particular, current QUIC defaults use a
5-second keepalive and 15-second idle timeout. Their packets, timer wakeups and CPU are part of the
production result. Connection establishment rate and admission-limit behaviour are separate tests;
they must not be folded into idle CPU.

The scale report must include:

- ready/target sessions and native start/end gauges;
- first-pass connect-and-probe and final application-ready setup time/rate;
- server cores and CPU per 1K sessions;
- incremental server RSS and KiB/session;
- packets, bytes and syscalls/s, including keepalive traffic;
- closures, drops, failures and RSS growth during the idle window;
- generator CPU/RSS, to verify that the load side was not the bottleneck.

`mixed-scale` establishes the same idle population, then runs `ihave-1x20` and `ihave-40x20` on 20
active neighbours. Compare 0, 10K and 100K idle backgrounds. This detects connection-table or timer
work accidentally entering the active fast path; it is more informative than idle CPU alone.

For credible 100K results, run generators on another host or on disjoint pinned cores. The current
`bench.sh` launcher is loopback-only, so a split-host run requires a manual or future distributed
launcher. Same-host loopback remains useful as a functional and allocator test, but shared CPU
contention is not a server-capacity result. Ramp through 1K and 10K first and budget total client plus server memory:
on the development Mac, quic-go used about 7.4 GiB on the server and 6.3 GiB on the generator at
100K, while Quinn's parity profile used about 6.4 GiB per side already at 10K. A Quinn 100K parity
attempt was therefore intentionally not run. This is configuration-sensitive: at 1K, Quinn retained
about 676 KiB/peer with TON's 4,096-stream profile but only about 43 KiB/peer with Quinn defaults. Record the `tuned`
column and do not present one profile as the other.

## Native QUIC parity drivers

[`quicbench-go`](./quicbench-go/README.md) and [`quicbench-rs`](./quicbench-rs/README.md) implement
the same server/client roles and workload model as the TON driver. They are suite implementations,
not one-process saturation microbenchmarks.

Each native server owns one UDP endpoint. Each client process owns one UDP endpoint and one or more
connections, distributed round-robin over consecutive server ports. Message workloads send one
fresh unidirectional stream per application message. Query workloads use one fresh bidirectional
stream per round trip. The native client performs an application-level probe on every connection
before publishing readiness, and both roles expose `/metrics` for the common measurement window.

```sh
(cd test/net/bench/quicbench-go && go build -o quicmsgbench-go .)
cargo build --release --manifest-path test/net/bench/quicbench-rs/Cargo.toml

test/net/bench/bench.sh -I quic-go -P quic -T query -c 2 -i 2 -s 7680 -r 1024
test/net/bench/bench.sh -I quinn -P quic -T message -n 20 -s 240 -m 6000 -B 800
```

The harness uses the same readiness, warm-up, scrape, CPU/RSS and validation phases for all three
QUIC implementations. Separate processes keep sender and receiver CPU attributable. Application
rates, delivery, MiB/s, errors, active connections and retained RSS use the same accounting rules.
The native rows are workload references, not pure library A/Bs: TON QUIC also includes TL framing,
peer authentication, actor routing and its buffer-ownership path, while the native drivers use a
small test framing protocol and test TLS. Native exporters deliberately do not invent wire
datagram/syscall or GSO batching counters, so those columns are `NA` in `summary.tsv`.

The Rust driver also has an optional macOS batched datapath build:

```sh
cargo build --release --features fast-apple \
  --manifest-path test/net/bench/quicbench-rs/Cargo.toml
```

## Saturation microbenchmarks

Separate from the harness: one process, one connection, a closed loop with a 16K receiver-lag
window, run to time. These answer "how many small messages can this stack move at all" in one
command, with none of the harness's pacing or readiness machinery — and therefore none of its
controlled-load semantics. Delivery below 100% is sampling at the time cutoff, not loss.

```sh
cmake-build-relwithdebinfo/bench-adnl-quic-message --quic -d 5
test/net/bench/quicbench-go/quicmsgbench-go -workload saturate -duration 5 -tuned
test/net/bench/quicbench-rs/target/release/quicmsgbench-rs --workload saturate -d 5 --tuned
```

`bench-adnl-quic-message` also has `--adnl`, `--quic-bidi` and `--quic-datagrams` arms and is the
only place the QUIC DATAGRAM path is exercised.

## QUIC egress A/B

Use separate immutable builds and interleave runs. The primary workload pair is:

- `ihave-1x20`, which protects the historical small-message padding regression;
- `ihave-40x20`, which gives every connection enough queued data to exercise packet filling/GSO.

Also use `bulk-large` as a dense per-connection transfer control. Run each profile with GSO enabled and
with `EXTRA=--no-gso`, at least five interleaved repetitions, at a rate every build delivers fully.

Compare CPU/delivered message and wire bytes/message together with:

- `segment/descriptor`, which proves GSO is active;
- `descriptor/send`, which measures `sendmmsg` batching;
- `dgram/send`, their composite effect;
- packets/nonempty flush, flushes above 16 packets and egress syscalls/message.

`dgram/send > 1` alone does not prove GSO because `sendmmsg` can also produce it. The sparse
`ihave-1x20` control must not show a wire-bytes/message jump.

Loopback has no meaningful loss or RTT. It is useful for CPU, batching and retained-state A/Bs, not
for congestion-control conclusions. macOS has neither Linux UDP GSO nor `sendmmsg`, so settle the
egress comparison on Linux.

This organization follows the separation used by mature tooling: [h2load](https://nghttp2.org/documentation/h2load.1.html)
uses explicit warm-up and measurement phases, [MsQuic secnetperf](https://microsoft.github.io/msquic/msquicdocs/src/perf/readme.html)
keeps workloads client-driven, and [Quinn](https://github.com/quinn-rs/quinn/blob/main/quinn-udp/benches/throughput.rs)
keeps transport batching microbenchmarks separate from end-to-end QUIC workloads.
