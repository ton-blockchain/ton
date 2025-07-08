## 2025.06 Update

1. ADNL and candidate broadcast optimization
2. [TVM version v11](./doc/GlobalVersions.md): new opcodes, and `c7` entry to improve developer experience. It also activates storage stats and `ihr_fee`  nullification.
3. Fixed `start_lt` of tick transactions [see details on 01.06.2025 incident](https://telegra.ph/Report-on-June-1-2025-Operation-Incident-06-02).
4. Introduction of persistent state sharding, as well as making serialization of large BOCs more deterministic
5. Emulator improvements: in get methods, set config from provided `c7`; allow retrieval of logs from emulator runs for get methods
6. Optimized package import for archive nodes

Besides the work of the core team, this update is based on the efforts of the RSquad team (deterministic large BOC serialization); AArayz, wy666444, Robinlzw, Lucian-code233 from TonBit (early discovery of the TVM 11 bug); @Skydev0h (uninitialized `BLOCKLT` in get methods); and @yma-het from TONWhales (emulator improvements).

