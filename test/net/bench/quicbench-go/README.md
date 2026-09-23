# quicmsgbench-go

The quic-go implementation of the shared transport workload contract in `../README.md`. It runs as
a separate server or client/generator process, supports idle, paced fire-and-forget message, and
closed-loop query workloads, and exposes `GET /metrics` for the common harness.

Build it and normally invoke it through `bench.sh`:

```sh
go build -o quicmsgbench-go .
../bench.sh -I quic-go -P quic -T message -n 20 -s 240 -m 4000 -B 20
../bench.sh -I quic-go -P quic -T query -c 2 -i 2 -s 64 -r 262144
```

One client process can own many connections over one UDP endpoint. `--connections N` is the total
connection count and `--servers N` distributes them round-robin over consecutive destination
ports. Setup is bounded by `--connect-inflight`, paced by `--connect-rate`, retries transient
refusals, and requires a readiness probe on every connection before printing `READY`. It repairs
connections lost before readiness and requires a stable second (5 seconds at 10K, 15 seconds at
100K) with no further loss.

`--tuned` is the parity profile used by the harness: 4,096 stream credits, TON-sized bounded flow
windows, and 5-second keepalive/15-second idle timeout. Omit it only for a separately labelled
quic-go-defaults run. `--threads` sets `GOMAXPROCS` explicitly.

The module is pinned to quic-go v0.61.0 and declares Go 1.25.
