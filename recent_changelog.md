## 2025.10 Update

1. [TVM version v12](./doc/GlobalVersions.md): full bounces, new `BTOS` and `HASHBU` instuctions, limit on contract size in masterchain.
2. Optimistic collation/validation: allow nodes to generate and check block candidates before previous block is fully signed (not fully activated yet).
3. Introduced custom block compression algorithm.
4. Overlay improvements: improved overlay discovery on shard configuration update, private externals in custom overlays.
5. Various improvements: session stats, telemetry in fast-sync overlay, earlier block broadcasts, limiting ttl for values in DHT, fixing search by utime in native blockexplorer, faster downloading candidates in validator session, parallelization of storing to cell_db, avoiding touching packfiles on startup.

Besides the work of the core team, this update is based on the efforts of the Tonstudio team: @hacker-volodya @Shvandre; and @mkiesel (avoiding touching packfiles on startup).
