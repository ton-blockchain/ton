## 2025.02 Update
1. Series of improvement/fixes for `Config8.version >= 9`, check [GlobalVersions.md](./doc/GlobalVersions.md)
2. Fix for better discovery of updated nodes' (validators') IPs: retry dht queries
3. Series of improvements for extra currency adoption: fixed c7 in rungetmethod, reserve modes
4. TVM: Fix processing continuation control data on deep jump
5. A few fixes of tl-b schemes: crc computation, incorrect tag for merkle proofs, advance_ext, NatWidth print
6. Emulator improvements: fix setting libraries,  extracurrency support
7. Increase of gas limit for unlocking highload-v2 wallets locked in the beginning of 2024
8. Validator console improvement: dashed names, better shard formats


Besides the work of the core team, this update is based on the efforts of  @dbaranovstonfi from StonFi(libraries in emulator), @Rexagon (ret on deep jumps), @tvorogme from DTon (`advance_ext`), Nan from Zellic (`stk_und` and JNI)
