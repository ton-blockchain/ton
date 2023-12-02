##2023.11 Update

1. New TVM Functionality. (Disabled by default)
2. A series of emulator improvements: libraries support, higher max stack size, etc
3. A series of tonlib and tonlib-cli improvements: wallet-v4 support, getconfig, showtransactions, etc
4. Changes to public libraries: now contract can not publish more than 256 libraries (config parameter) and contracts can not be deployed with public libraries in initstate (instead contracts need explicitly publish all libraries)
5. Changes to storage due payment: now due payment is collected in Storage Phase, however for bouncable messages fee amount can not exceed balance of account prior to message.


Besides the work of the core team, this update is based on the efforts of @aleksej-paschenko (emulator improvements), @akifoq (security improvements), Trail of Bits auditor as well as all participants of [TEP-88 discussion](https://github.com/ton-blockchain/TEPs/pull/88).
