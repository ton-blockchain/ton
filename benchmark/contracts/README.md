# Benchmark contract artifacts

Code cells used by `bench-state-gen` / `bench-spam` (see `benchmark/DESIGN.md`).

## Files

| file | root cell hash (hex) | depth | cells |
|---|---|---|---|
| `wallet-v5.code.boc` | `20834b7b72b112147e1b2fb457b84e74d1a30f04f737d4f62a668e9552d2b72f` | 6 | 20 |
| `jetton-wallet.code.boc` | `beb0683ebeb8927fe9fc8ec0a18bc7dd17899689825a121eab46c5a3a860d0ce` | 7 | 17 |
| `jetton-minter.code.boc` | `13f5d7a316c6d76e1053e88ac59b5de65a072a388451371dc5c5becbba13f50e` | 4 | 11 |

`contracts.json` holds the same hashes plus jetton op-codes
(from `crypto/func/auto-tests/legacy_tests/jetton-wallet/imports/op-codes.fc`):
transfer `0xf8a7ea5`, internal_transfer `0x178d4519`, excesses `0xd53276db`,
transfer_notification `0x7362d09c`, burn `0x595f07bc`, burn_notification `0x7bdd97de`.

## Storage layouts

Jetton minter (`crypto/func/auto-tests/legacy_tests/jetton-minter/jetton-minter.fc`):

```
storage#_ total_supply:Coins admin_address:MsgAddress content:^Cell jetton_wallet_code:^Cell = Storage;
```

Jetton wallet (`crypto/func/auto-tests/legacy_tests/jetton-wallet/jetton-wallet.fc`):

```
storage#_ balance:Coins owner_address:MsgAddressInt jetton_master_address:MsgAddressInt jetton_wallet_code:^Cell = Storage;
```

Wallet v5 data cell (per DESIGN.md):
`1(bit) | seqno=0:u32 | wallet_id=0:u32 | pubkey:256 | 0(bit, empty ext dict)` (322 bits).

## How each .boc was produced

`wallet-v5.code.boc`: decoded from the `hex` field of
`/mnt/bench/work/contracts/wallet_v5.compiled.json` (pre-built artifact);
root hash verified against its `hash` field.

`jetton-wallet.code.boc` / `jetton-minter.code.boc`: compiled from the
in-repo legacy test sources with the in-repo toolchain (build dir `build/`):

```sh
build/crypto/func -o /tmp/jetton-wallet.fif -SPA \
    crypto/func/auto-tests/legacy_tests/jetton-wallet/jetton-wallet.fc
build/crypto/func -o /tmp/jetton-minter.fif -SPA \
    crypto/func/auto-tests/legacy_tests/jetton-minter/jetton-minter.fc
```

(`#include "imports/..."` paths resolve relative to each source file.)

Then each generated fif was assembled and the code cell serialized via a
small wrapper fif:

```fift
"Asm.fif" include
"/tmp/jetton-wallet.fif" include
boc>B "benchmark/contracts/jetton-wallet.code.boc" B>file
```

run as:

```sh
build/crypto/fift -I crypto/fift/lib:crypto/smartcont save-jw.fif
```

Sanity check (root hash / depth / distinct cell count) was done by loading
each .boc with `pytoniq_core.Cell.one_from_boc` from the repo venv.
