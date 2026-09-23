# quicmsgbench-rs

The Quinn implementation of the shared transport workload contract in `../README.md`. It has
separate server and client/generator roles, idle, paced fire-and-forget message, and closed-loop
query workloads, plus an HTTP metrics endpoint consumed by the common harness.

Build it and normally invoke it through `bench.sh`:

```sh
cargo build --release --locked --offline
../bench.sh -I quinn -P quic -T message -n 20 -s 240 -m 6000 -B 800
../bench.sh -I quinn -P quic -T query -c 2 -i 2 -s 64 -r 262144
```

`--connections N` connections share one Quinn endpoint and are spread round-robin over
`--servers N` consecutive ports. `--connect-inflight` bounds concurrent handshakes and
`--connect-rate` controls the ramp. Every connection completes a bidirectional readiness probe
and survives a 1-second stability wait (5 seconds at 10K, 15 seconds at 100K) before the client
prints `READY`; the run fails if any connection closes during that wait.

`--tuned` selects the parity profile: 4,096 stream credits, TON-sized bounded flow windows, and
5-second keepalive/15-second idle timeout. `--threads` fixes the Tokio worker count. `--no-gso`
disables segmentation offload for an explicitly labelled control run.
QUIC DATAGRAM is disabled because the shared workloads use streams and TON's compared path does not
advertise DATAGRAM support.

The optional `fast-apple` feature enables Quinn UDP's macOS batched datapath:

```sh
cargo build --release --locked --offline --features fast-apple
```
