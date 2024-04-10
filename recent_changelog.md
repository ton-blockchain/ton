## 2024.04 Update

1. Emulator: Single call optimized runGetMethod added
2. Tonlib: a series of proof improvements, also breaking Change in `liteServer.getAllShardsInfo` method (see below)
3. DB: usage statistics now collected, outdated persistent states are not serialized
4. LS: fast `getOutMsgQueueSizes` added, preliminary support of non-final block requests
5. Network: lz4 compression of block candidates (disabled by default).
6. Overlays: add custom overlays
7. Transaction Executor: fixed issue with due_payment collection

* `liteServer.getAllShardsInfo` method was updated for better efficiency. Previously, field proof contained BoC with two roots: one for BlockState from block's root and another for ShardHashes from BlockState. Now, it returns a single-root proof BoC, specifically the merkle proof of ShardHashes directly from the block's root, streamlining data access and integrity. Checking of the proof requires to check that ShardHashes in the `data` correspond to ShardHashes from the block.

Besides the work of the core team, this update is based on the efforts of @akifoq (due_payment issue).
