## 2026.11 Update

1. Added public rebroadcaster mode.
2. Improved overlay broadcasts.
3. Upgraded validator registry contract: new voting system for selecting collators and members of protected overlays, store ADNL address lists on-chain.
4. [Global version 18](./doc/GlobalVersions.md).
5. Added anti-dos features to QUIC implementation.
6. Legacy RLDP2 is completely replaced by QUIC.
7. Removed unsupported rldp-http-proxy, storage, payment channels from the mainrepo. Official implementations can be found at: [Storage](https://github.com/xssnick/tonutils-storage), [Proxy](https://github.com/xssnick/Tonutils-Proxy), [Reverse proxy](https://github.com/tonutils/reverse-proxy), [Payment network](https://github.com/xssnick/ton-payment-network).
8. Upgraded dependencies: secp256k1 v0.8.0, libsodium 1.0.22-RELEASE, RocksDB v11.13.2, LZ4 v1.11.1, zlib v1.4, OpenSSL v3.6.5.
9. Various other changes and stability fixes.

Besides the work of the core team, this update also includes contributions from:
- Multiple bug-bounty hunters.
- [dmshagov](https://github.com/dmshagov) - bugfix for validator key management.
