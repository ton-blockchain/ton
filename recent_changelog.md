## 2024.12 Update

1. FunC 0.4.6: Fix in try/catch handling, fixing pure flag for functions stored in variables
2. Merging parts of Accelerator: support of specific shard monitoring, archive/liteserver slice format, support for partial liteservers, proxy liteserver, on-demand neighbour queue loading
3. Fix of asynchronous cell loading
4. Various improvements: caching certificates checks, better block overloading detection, `_malloc` in emulator
5. Introduction of telemetry in overlays
6. Use non-null local-id for tonlib-LS interaction - mitigates MitM attack.
7. Adding `SECP256K1_XONLY_PUBKEY_TWEAK_ADD`, `SETCONTCTRMANY` instructions to TVM (activated by `Config8.version >= 9`)
8. Private keys export via validator-engine-console - required for better backups
9. Fix proof checking in tonlib, `hash` in `raw.Message` in tonlib_api

Besides the work of the core team, this update is based on the efforts of OtterSec and LayerZero (FunC), tg:@throwunless (FunC), Aviv Frenkel and Dima Kogan from Fordefi (LS MitM), @hacker-volodya (Tonlib), OKX team (async cell loading), @krigga (emulator)
