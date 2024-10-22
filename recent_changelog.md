## 2024.10 Update

1. Parallel write to celldb: substantial improvement of sync and GC speed, especially with slow disks.
2. Decreased network traffic: only first block candidate is sent optimistically.
3. Improved channel creation and dht lookups, introduction of semi-private overlays
4. New LS dispatch queue related methods and improvement security
5. Fixing recursion in TVM continuations
6. Improved stats for actors, validator sessions, perf counters, overlays, adnl, rocksdb
7. Migration to C++20
8. Improved block size estimates: account for depth in various structures
9. Fix bug with `<<` optimization in FunC
10. Minor changes of TVM which will be activated by `Config8.version >= 9`
11. Multiple minor improvements

Besides the work of the core team, this update is based on the efforts of @krigga (emulator), Arayz @ TonBit (LS security, TVM recursion), @ret2happy (UB in BLST).


