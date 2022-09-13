## 05.2022 Update
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
## 08.2022 Update
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
