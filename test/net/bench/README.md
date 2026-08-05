# Transport benchmarks

One suite that measures TON's transports (QUIC, ADNL, RLDP2) and two reference stacks (quic-go,
quinn) on identical workloads: gossip-shaped paced messages (IHAVE fan-out, fan-in), query/response
(bulk), idle connections, and saturation ceilings. Separate server and client processes, readiness
probes, warm-up, fixed measurement windows, exporters scraped on both sides.

## Run

```sh
ninja -C cmake-build-relwithdebinfo bench-rldp bench-adnl-quic-message   # needs -DTON_USE_JEMALLOC=ON
(cd test/net/bench/quicbench-go && go build -o quicmsgbench-go .)
cargo build --release --locked --manifest-path test/net/bench/quicbench-rs/Cargo.toml

test/net/bench/suite.sh        # runs every implementation it finds built; prints a median summary
REPETITIONS=3 test/net/bench/suite.sh
```

One workload: `bench.sh -I quinn -P quic -T message -n 20 -s 240 -m 4000 -B 20`.
Transport-only ceiling, one command: `bench-adnl-quic-message --quic` (also `--adnl`,
`--quic-bidi`, `--quic-datagrams`).

Workload definitions, measurement contract and validity rules: [METHODOLOGY.md](METHODOLOGY.md).
Pinned numbers from the reference Linux host: [RESULTS.md](RESULTS.md).

## Main optimizations this suite validated

- **Pacer charged once per flush + conditional GSO padding** — ngtcp2's pacing gate is consuming;
  per-packet charging serialized every flush to one packet. With batch-continuing padding:
  egress syscalls per datagram 0.60 → 0.05 on loopback, batching preserved at 50 ms RTT.
- **Stream-credit return batching** — one MAX_STREAMS per 64 closed streams instead of per message
  (each was ack-eliciting): sparse fan-out 2.0 → 1.5 datagrams/msg, fan-out ceiling +30%.
- **jemalloc** — cross-thread alloc/free is glibc's worst case and was 27% of CPU: streams +58%,
  datagrams +72% throughput. `TON_USE_JEMALLOC` is OFF by default; benchmarks must build with it.
- **Messages on unidirectional streams** (with old-peer bidi fallback) and **single-chunk delivery
  without reassembly state** — one stream, no application receipt, no copy on the common path.
- **Keyed connection-id hashing** (SipHash-1-3) — wire-chosen keys can't flood a bucket; hashed
  maps replace ordered ones on the per-datagram lookup path.
- **RFC 9221 datagrams** as a benchmark-only option — the fastest arm measured (~800–890k msg/s,
  2x any stream arm); production use stays gated on queue fairness and backpressure work.

Net effect on the reference host: TON QUIC leads every saturation workload, beats quic-go
everywhere except lightly-paced messages, and runs streams within ~15% of quinn; the remaining
paced-load CPU gap to quinn is actor thread-handoff cost, left for a future series.
