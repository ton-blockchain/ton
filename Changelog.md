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

## 2024.08 Update

1. Introduction of dispatch queues, message envelopes with transaction chain metadata, and explicitly stored msg_queue size, which will be activated by `Config8.version >= 8` and new `Config8.capabilities` bits: `capStoreOutMsgQueueSize`, `capMsgMetadata`, `capDeferMessages`. 
2. A number of changes to transcation executor which will activated for `Config8.version >= 8`:
    - Check mode on invalid `action_send_msg`. Ignore action if `IGNORE_ERROR` (+2) bit is set, bounce if `BOUNCE_ON_FAIL` (+16) bit is set.
    - Slightly change random seed generation to fix mix of `addr_rewrite` and `addr`.
    - Fill in `skipped_actions` for both invalid and valid messages with `IGNORE_ERROR` mode that can't be sent.
    - Allow unfreeze through external messages.
    - Don't use user-provided `fwd_fee` and `ihr_fee` for internal messages.
3. A few issues with broadcasts were fixed: stop on receiving last piece, response to AdnlMessageCreateChannel
4. A number of fixes and improvements for emulator and tonlib: correct work with config_addr, not accepted externals, bounces, debug ops gas consumption, added version and c5 dump, fixed tonlib crashes
5. Added new flags and commands to the node, in particular `--fast-state-serializer`, `getcollatoroptionsjson`, `setcollatoroptionsjson`

Besides the work of the core team, this update is based on the efforts of @krigga (emulator), stonfi team, in particular @dbaranovstonfi and @hey-researcher (emulator), and  @loeul, @xiaoxianBoy, @simlecode (typos in comments and docs).



## 2024.06 Update

1. Make Jemalloc default allocator
2. Add candidate broadcasting and caching
3. Limit per address speed for external messages broadcast by reasonably large number 
4. Overlay improvements: fix dropping peers in small custom overlays, fix wrong certificate on missed keyblocks
5. Extended statistics and logs for celldb usage, session stats, persistent state serialization
6. Tonlib and explorer fixes
7. Flags for precize control of Celldb: `--celldb-cache-size`, `--celldb-direct-io` and `--celldb-preload-all`
8. Add valiator-console command to stop persistent state serialization
9. Use `@` path separator for defining include path in fift and create-state utilities on Windows only.


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

## 2024.03 Update

1. Preparatory (not enabled yet) code for pre-compiled smart-contract.
2. Minor fixes for fee-related opcodes.

## 2024.02 Update

1. Improvement of validator synchronisation:
   * Better handling of block broadcasts -> faster sync
   * Additional separate overlay among validators as second option for synchronisation
2. Improvements in LS:
   * c7 and library context is fully filled up for server-side rungetmethod
   * Cache for runmethods and successfull external messages
   * Logging of LS requests statistic
3. Precise control of open files:
   * almost instantaneous validator start
   * `--max-archive-fd` option
   * autoremoval of not used temp archive files
   * `--archive-preload-period` option
4. Preparatory (not enabled yet) code for addition on new TVM instructions for cheaper fee calculation onchain.

## 2024.01 Update

1. Fixes in how gas in transactions on special accounts is accounted in block limit. Previously, gas was counted as usual, so to conduct elections that costs >30m gas block limit in masterchain was set to 37m gas. To lower the limit for safety reasons it is proposed to caunt gas on special accounts separately. Besides `gas_max` is set to `special_gas_limit` for all types of transactions on special accounts. New behavior is activated through setting `version >= 5` in `ConfigParam 8;`.
   * Besides update of config temporally increases gas limit on `EQD_v9j1rlsuHHw2FIhcsCFFSD367ldfDdCKcsNmNpIRzUlu` to `special_gas_limit`, see [details](https://t.me/tonstatus/88).
2. Improvements in LS behavior
   * Improved detection of the state with all shards applied to decrease rate of `Block is not applied` error
   * Better error logs: `block not in db` and `block is not applied` separation
   * Fix error in proof generation for blocks after merge
   * Fix most of `block is not applied` issues related to sending too recent block in Proofs
   * LS now check external messages till `accept_message` (`set_gas`).
3. Improvements in DHT work and storage, CellDb, config.json ammendment, peer misbehavior detection, validator session stats collection, emulator.
4. Change in CTOS and XLOAD behavior activated through setting `version >= 5` in `ConfigParam 8;`:
   * Loading "nested libraries" (i.e. a library cell that points to another library cell) throws an exception.
   * Loading a library consumes gas for cell load only once (for the library cell), not twice (both for the library cell and the cell in the library).
   * `XLOAD` now works differently. When it takes a library cell, it returns the cell that it points to. This allows loading "nested libraries", if needed.

Besides the work of the Core team, this update is based on the efforts of @XaBbl4 (peer misbehavior detection) and @akifoq (CTOS behavior and gas limit scheme for special accounts).

## 2023.12 Update

1. Optimized message queue handling, now queue cleaning speed doesn't depend on total queue size
     * Cleaning delivered messages using lt augmentation instead of random search / consequtive walk
     * Keeping root cell of queue message in memory until outdated (caching)
2. Changes to block collation/validation limits
3. Stop accepting new external message if message queue is overloaded
4. Introducing conditions for shard split/merge based on queue size

Read [more](https://blog.ton.org/technical-report-december-5-inscriptions-launch-on-ton) on that update.

## 2023.11 Update

1. New TVM Functionality. (Disabled by default)
2. A series of emulator improvements: libraries support, higher max stack size, etc
3. A series of tonlib and tonlib-cli improvements: wallet-v4 support, getconfig, showtransactions, etc
4. Changes to public libraries: now contract can not publish more than 256 libraries (config parameter) and contracts can not be deployed with public libraries in initstate (instead contracts need explicitly publish all libraries)
5. Changes to storage due payment: now due payment is collected in Storage Phase, however for bouncable messages fee amount can not exceed balance of account prior to message.


Besides the work of the core team, this update is based on the efforts of @aleksej-paschenko (emulator improvements), @akifoq (security improvements), Trail of Bits auditor as well as all participants of [TEP-88 discussion](https://github.com/ton-blockchain/TEPs/pull/88).

## 2023.10 Update
1. A series of additional security checks in node: special cells in action list, init state in external messages, peers data prior to saving to disk.
2. Human-readable timestamps in explorer

Besides the work of the core team, this update is based on the efforts of @akifoq and @mr-tron.

## 2023.06 Update
1. (disabled by default) New deflation mechanisms: partial fee burning and blackhole address
2. Storage-contract improvement

Besides the work of the core team, this update is based on the efforts of @DearJohnDoe from Tonbyte (Storage-contract improvement).

## 2023.05 Update
1. Archive manager optimization
2. A series of catchain (basic consensus protocol) security improvements
3. Update for Fift libraries and FunC: better error-handling, fixes for `catch` stack recovery
4. A series of out message queue handling optimization (already deployed during emergency upgrades between releases)
5. Improvement of binaries portability

Besides the work of the core team, this update is based on the efforts of @aleksej-paschenko (portability improvement), [Disintar team](https://github.com/disintar/) (archive manager optimization) and [sec3-service](https://github.com/sec3-service) security auditors (funC improvements).

## 2023.04 Update
1. CPU load optimization: previous DHT reconnect policy was too aggressive
2. Network throughput improvements: granular control on external message broadcast, optimize celldb GC, adjust state serialization and block downloading timings, rldp2 for states and archives 
3. Update for Fift (namespaces) and Fift libraries (list of improvements: https://github.com/ton-blockchain/ton/issues/631)
4. Better handling of incorrect inputs in funC: fix UB and prevent crashes on some inputs, improve optimizing int consts and unused variables in FunC, fix analyzing repeat loop. FunC version is increase to 0.4.3.
5. `listBlockTransactionsExt` in liteserver added
6. Tvm emulator improvements

Besides the work of the core team, this update is based on the efforts of @krigga (tvm emulator improvement), @ex3ndr (`PUSHSLICE` fift-asm improvement) and [sec3-service](https://github.com/sec3-service) security auditors (funC improvements).

## 2023.03 Update
1. Improvement of ADNL connection stability
2. Transaction emulator support and getAccountStateByTransaction method
3. Fixes of typos, undefined behavior and timer warnings
4. Handling incorrect integer literal values in funC; funC version bumped to 0.4.2
5. FunC Mathlib

## 2023.01 Update
1. Added ConfigParam 44: `SuspendedAddressList`. Upon being set this config suspends initialisation of **uninit** addresses from the list for given time.
2. FunC: `v0.4.1` added pragmas for precise control of computation order
3. FunC: fixed compiler crashes for some exotic inputs
4. FunC: added legacy tester, a collection of smart-contracts which is used to check whether compilator update change compilation result
5. Improved archive manager: proper handling of recently garbage-collected blocks

## 2022.12 Update
Node update:
1. Improvements of ton-proxy: fixed few bugs, improved stability
2. Improved collator/validator checks, added optimization of storage stat calculation, generation and validation of new blocks is made safer
3. Some previously hard-coded parameters such as split/merge timings, max sizes and depths of internal and external messages, and others now can be updated by validators through setting ConfigParams. Max contract size added to configs.
4. Tonlib: updated raw.getTransactions (now it contains InitState), fixed long bytestrings truncation
5. abseil-cpp is updated to newer versions
6. Added configs for Token Bridge
7. LiteServers: a few bug fixes, added liteServer.getAccountStatePrunned method, improved work with not yet applied blocks.
8. Improved DHT: works for some NAT configurations, optimized excessive requests, added option for DHT network segregation.
9. FunC v0.4.0: added try/catch statements, added throw_arg functions, allowed in-place modification of global variables, forbidden ambiguous modification of local variables after it's usage in the same expression.
10. TON Storage: added storage-daemon (create, download bag of Files, storage-provider staff), added storage-daemon-cli

Besides the work of the core team, this update is based on the efforts of @vtamara (help with abseil-cpp upgrade), @krigga(in-place modification of global variables) and third-party security auditors.

## 2022.10 Update
* Added extended block creation and general perfomance stats gathering
* Forbidden report data on blocks not committed to the master chain for LS
* Improved debug in TVM
* FunC 0.3.0: multi-line asms, bitwise operations for constants, duplication of identical definition for constants and asms now allowed
* New tonlib methods: sendMessageReturnHash, getTransactionsV2, getMasterchainBlockSignatures, getShardBlockProof, getLibraries.
* Fixed bugs related to invalid TVM output (c4, c5, libaries) and non-validated network data; avoided too deep recursion in libraries loading
* Fixed multiple undefined behavior issues
* Added build of FunC and Fift to WASM

Besides the work of the core team, this update is based on the efforts of @tvorogme (debug improvements), @AlexeyFSL (WASM builds)  and third-party security auditors.

## 2022.08 Update
* Blockchain state serialization now works via separate db-handler which simplfies memory clearing after serialization
* CellDB now works asynchronously which substantially increase database access throughput
* Abseil-cpp and crc32 updated: solve issues with compilation on recent OS distributives
* Fixed a series of UBs and issues for exotic endianness hosts
* Added detailed network stats for overlays (can be accessed via `validator-console`)
* Improved auto-builds for wide range of systems.
* Added extended error information for unaccepted external messages: `exit_code` and TVM trace (where applicable).
* [Improved catchain DoS resistance](https://github.com/ton-blockchain/ton/blob/master/doc/catchain-dos.md)
* A series of FunC improvements, summarized [here](https://github.com/ton-blockchain/ton/pull/378)
#### Update delay
Update coincided with persistent state serialization event which lead to block production speed deterioration (issue substantially mitigated in update itself). This phenomena was aggravated by the fact that after update some validators lost ability to participate in block creation. The last was caused by threshold based hardcoded protocol version bump, where threshold was set in such manner (based on block height with value higher than 9m), that it eluded detection in private net tests. The update was temporarily paused and resumed after persistent state serialization ended and issues with block creation were resolved.

Besides the work of the core team, this update is based on the efforts of @awesome-doge (help with abseil-cpp upgrade), @rec00rsiff (noted issues for exotic endianess and implemented network stats) and third-party security auditors.

## 2022.05 Update
* Initial synchronization improved: adjusted timeouts for state download and the way of choosing which state to download. Nodes with low network speed and/or bad connectivity will synchronize faster and consistently.
* Improved peer-to-peer network stability and DDoS resistance: now peers will only relay valid messages to the network. Large messages, which require splitting for relaying, will be retranslated as well, but only after the node gets all parts, and reassembles and checks them. Validators may sign certificates for network peers, which allow relaying large messages by parts without checks. It is used now by validators to faster relay new blocks. Sign and import certificate commands are exposed via `validator-engine-console`.
* Fixed some rare edge cases in TVM arithmetic operations related to big numbers (`2**63+`)
* Improved fixes used to combat wrong activate-destruct-activate contract behavior last November.
* Improved tonlib: support libraries (with client-side caching), getmethods completely fill c7 register, getmethods support slice arguments, improved messages listing for transactions, added extended block header params, added getConfig method.
* RocksDB updated to a newer version.
* Improved persistent state serialization: memory usage during serialization was optimized; the start of serialization on different nodes was sparsed.
* FunC update: support for string literals and constants (including precompiled constant expressions), semver, `include` expressions.
* Fixed rarely manifested bugs in `Asm.fif`.
* LiteClient supports key as cli parameter.
* Improved Liteserver DoS resistance for running getmethods.

Besides the work of the core team, this update is based on the efforts of @tvorogme (added support for slice arguments and noted bugs in Asm.fif), @akifoq (fixed bug in Asm.fif), @cryshado (noted strange behavior of LS, which, upon inspection, turned out to be a vector of DoS attack).


