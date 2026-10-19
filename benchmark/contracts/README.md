# Benchmark contract artifacts

Code cells used by `bench-state-gen` / `bench-spam` (see `benchmark/DESIGN.md`).

## Files

| file | root cell hash (hex) | depth | cells |
|---|---|---|---|
| `wallet-v5.code.boc` | `20834b7b72b112147e1b2fb457b84e74d1a30f04f737d4f62a668e9552d2b72f` | 6 | 20 |
| `jetton-wallet.code.boc` | `d3825ce0109ecb6f6cd125b7a28258f488041153eb12e02c42045c4f05044573` | 5 | 12 |
| `jetton-minter.code.boc` | `13f5d7a316c6d76e1053e88ac59b5de65a072a388451371dc5c5becbba13f50e` | 4 | 11 |
| `fat.code.boc` | `98c82e8b7c3bd95ba9f083520dd7acb187f20eb9577085fca59924b8832097f2` | 2 | 4 |

`contracts.json` holds the same hashes plus jetton op-codes: transfer `0xf8a7ea5`,
internal_transfer `0x178d4519`, excesses `0xd53276db`, transfer_notification `0x7362d09c`,
burn `0x595f07bc`, burn_notification `0x7bdd97de`.

## Storage layouts

Prepaid jetton wallet (`jetton-wallet.code.boc`, from the Tolk `JettonWallet` — no code ref in data;
the wallet is born with a prepaid balance so its address derives from that initial balance):

```
storage#_ jetton_balance:Coins owner_address:MsgAddressInt jetton_master_address:MsgAddressInt = Storage;
```

Fat load-target (`fat.code.boc`, from the Tolk `Fat`): a large storage dict so collating a message to it
forces a full storage-stat walk; a flagged transfer_notification bumps the nonce to defeat the cache.

```
storage#_ nonce:uint64 big_dict:^Cell = Storage;
```

Jetton minter (`jetton-minter.code.boc`, still the legacy FunC contract — used only for its deterministic
address, embedded as the `minter` field of every jetton wallet; the minter itself is never invoked):

```
storage#_ total_supply:Coins admin_address:MsgAddress content:^Cell jetton_wallet_code:^Cell = Storage;
```

Wallet v5 data cell (per DESIGN.md):
`1(bit) | seqno=0:u32 | wallet_id=0:u32 | pubkey:256 | 0(bit, empty ext dict)` (322 bits).

## How each .boc was produced

`wallet-v5.code.boc`: decoded from the `hex` field of a pre-built `wallet_v5.compiled.json`;
root hash verified against its `hash` field.

`jetton-wallet.code.boc` (prepaid) and `fat.code.boc`: the `code_boc64` of the Tolk `JettonWallet` /
`Fat` contracts built with the `acton` CLI (`jetton-prepaid/build/{JettonWallet,Fat}.json`), decoded to
a raw BoC; root hash verified against each build's `hash` field.

`jetton-minter.code.boc`: compiled from the in-repo legacy test source with the in-repo toolchain:

```sh
build/crypto/func -o /tmp/jetton-minter.fif -SPA \
    crypto/func/auto-tests/legacy_tests/jetton-minter/jetton-minter.fc
```

then assembled and serialized via a small `Asm.fif` wrapper and `build/crypto/fift`.
