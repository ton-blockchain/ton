# QUIC RPK Rust Interop

This crate contains a small Quinn-based raw-public-key interop pair:

- `rpk-server`: HTTP-over-QUIC demo server
- `rpk-client`: interop client with path-migration and CID-rotation checks

Build:

```bash
cargo build --release --bin rpk-server --bin rpk-client
```

Run the Rust server:

```bash
./target/release/rpk-server 127.0.0.1:19999
```

The server prints its Ed25519 public key in hex. Use that to run the Rust client:

```bash
./target/release/rpk-client 127.0.0.1:19999 <server-pubkey>
```

You can also use the C++ example client against the Rust server:

```bash
../../cmake-build-debug/quic/quic-example-client --host 127.0.0.1 --port 19999
```

Full mixed C++/Rust interop coverage is exercised by:

```bash
../../test-quic-interop.sh
```
