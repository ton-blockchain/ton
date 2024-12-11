# Global versions
Global version is a parameter specified in `ConfigParam 8` ([block.tlb](https://github.com/ton-blockchain/ton/blob/master/crypto/block/block.tlb#L595)).
Various features are enabled depending on the global version.

## Version 4

### New TVM instructions
* `PREVMCBLOCKS`, `PREVKEYBLOCK`
* `GLOBALID`
* `HASHEXT(A)(R)`
* `ECRECOVER`
* `SENDMSG`
* `RUNVM`, `RUNVMX`
* `GASCONSUMED`
* `RIST255_...` instructions
* `BLS_...` instructions
* `P256_CHKSIGNS`, `P256_CHKSIGNU`

### Division
[Division instruction](https://ton.org/docs/learn/tvm-instructions/instructions#52-division) can add a number to the
intermediate value before division (e.g. `(xy+w)/z`).

### Stack operations
* Arguments of `PICK`, `ROLL`, `ROLLREV`, `BLKSWX`, `REVX`, `DROPX`, `XCHGX`, `CHKDEPTH`, `ONLYTOPX`, `ONLYX` are now unlimited.
* `ROLL`, `ROLLREV`, `BLKSWX`, `REVX`, `ONLYTOPX` consume more gas when arguments are big.

### c7 tuple
**c7** tuple extended from 10 to 14 elements:
* **10**: code of the smart contract.
* **11**: value of the incoming message.
* **12**: fees collected in the storage phase.
* **13**: information about previous blocks.

### Action phase
* If "send message" action fails, the account is required to pay for processing cells of the message.
* Flag +16 in actions "Send message", "Reserve", "Change library" causes bounce if action fails.

### Storage phase
* Unpaid storage fee is now saved to `due_payment`

## Version 5

### Gas limits
Version 5 enables higher gas limits for special contracts.

* Gas limit for all transactions on special contracts is set to `special_gas_limit` from `ConfigParam 20` (which is 35M at the moment of writing). 
Previously only ticktock transactions had this limit, while ordinary transactions had a default limit of `gas_limit` gas (1M).
* Gas usage of special contracts is not taken into account when checking block limits. This allows keeping masterchain block limits low
while having high gas limits for elector.
* Gas limit on `EQD_v9j1rlsuHHw2FIhcsCFFSD367ldfDdCKcsNmNpIRzUlu` is increased to `special_gas_limit * 2` until 2024-02-29.
See [this post](https://t.me/tonstatus/88) for details.

### Loading libraries
* Loading "nested libraries" (i.e. a library cell that points to another library cell) throws an exception.
* Loading a library consumes gas for cell load only once (for the library cell), not twice (both for the library cell and the cell in the library).
* `XLOAD` now works differently. When it takes a library cell, it returns the cell that it points to. This allows loading "nested libraries", if needed.

## Version 6

### c7 tuple
**c7** tuple extended from 14 to 17 elements:
* **14**: tuple that contains some config parameters as cell slices. If the parameter is absent from the config, the value is null. Asm opcode: `UNPACKEDCONFIGTUPLE`.
  * **0**: `StoragePrices` from `ConfigParam 18`. Not the whole dict, but only the one StoragePrices entry (one which corresponds to the current time).
  * **1**: `ConfigParam 19` (global id).
  * **2**: `ConfigParam 20` (mc gas prices).
  * **3**: `ConfigParam 21` (gas prices).
  * **4**: `ConfigParam 24` (mc fwd fees).
  * **5**: `ConfigParam 25` (fwd fees).
  * **6**: `ConfigParam 43` (size limits).
* **15**: "[due payment](https://github.com/ton-blockchain/ton/blob/8a9ff339927b22b72819c5125428b70c406da631/crypto/block/block.tlb#L237)" - current debt for storage fee (nanotons). Asm opcode: `DUEPAYMENT`.
* **16**: "precompiled gas usage" - gas usage for the current contract if it is precompiled (see `ConfigParam 45`), `null` otherwise. Asm opcode: `GETPRECOMPILEDGAS`.

### New TVM instructions

#### Fee calculation
* `GETGASFEE` (`gas_used is_mc - price`) - calculates gas fee.
* `GETSTORAGEFEE` (`cells bits seconds is_mc - price`) - calculates storage fees (only current StoragePrices entry is used).
* `GETFORWARDFEE` (`cells bits is_mc - price`) - calculates forward fee.
* `GETPRECOMPILEDGAS` (`- x`) - returns gas usage for the current contract if it is precompiled, `null` otherwise.
* `GETORIGINALFWDFEE` (`fwd_fee is_mc - orig_fwd_fee`) - calculate `fwd_fee * 2^16 / first_frac`. Can be used to get the original `fwd_fee` of the message.
* `GETGASFEESIMPLE` (`gas_used is_mc - price`) - same as `GETGASFEE`, but without flat price (just `(gas_used * price) / 2^16`).
* `GETFORWARDFEESIMPLE` (`cells bits is_mc - price`) - same as `GETFORWARDFEE`, but without lump price (just `(bits*bit_price + cells*cell_price) / 2^16`).

`gas_used`, `cells`, `bits`, `time_delta` are integers in range `0..2^63-1`.

#### Cell operations
Operations for working with Merkle proofs, where cells can have non-zero level and multiple hashes.
* `CLEVEL` (`cell - level`) - returns level of the cell.
* `CLEVELMASK` (`cell - level_mask`) - returns level mask of the cell.
* `i CHASHI` (`cell - hash`) - returns `i`th hash of the cell.
* `i CDEPTHI` (`cell - depth`) - returns `i`th depth of the cell.
* `CHASHIX` (`cell i - hash`) - returns `i`th hash of the cell.
* `CDEPTHIX` (`cell i - depth`) - returns `i`th depth of the cell.

`i` is in range `0..3`.

### Other changes
* `GLOBALID` gets `ConfigParam 19` from the tuple, not from the config dict. This decreases gas usage.
* `SENDMSG` gets `ConfigParam 24/25` (message prices) from the tuple, not from the config dict, and also uses `ConfigParam 43` to get max_msg_cells.


## Version 7

[Explicitly nullify](https://github.com/ton-blockchain/ton/pull/957/files) `due_payment` after due reimbursment.

## Version 8

- Check mode on invalid `action_send_msg`. Ignore action if `IGNORE_ERROR` (+2) bit is set, bounce if `BOUNCE_ON_FAIL` (+16) bit is set.
- Slightly change random seed generation to fix mix of `addr_rewrite` and `addr`.
- Fill in `skipped_actions` for both invalid and valid messages with `IGNORE_ERROR` mode that can't be sent.
- Allow unfreeze through external messages.
- Don't use user-provided `fwd_fee` and `ihr_fee` for internal messages.

## Version 9

### New TVM instructions
- `SECP256K1_XONLY_PUBKEY_TWEAK_ADD` (`key tweak - 0 or f x y -1`) - performs [`secp256k1_xonly_pubkey_tweak_add`](https://github.com/bitcoin-core/secp256k1/blob/master/include/secp256k1_extrakeys.h#L120).
`key` and `tweak` are 256-bit unsigned integers. 65-byte public key is returned as `uint8 f`, `uint256 x, y` (as in `ECRECOVER`). Gas cost: `1276`.
- `mask SETCONTCTRMANY` (`cont - cont'`) - takes continuation, performs the equivalent of `c[i] PUSHCTR SWAP c[i] SETCONTCNR` for each `i` that is set in `mask` (mask is in `0..255`).
- `SETCONTCTRMANYX` (`cont mask - cont'`) - same as `SETCONTCTRMANY`, but takes `mask` from stack.

### Other changes
- Fix `RAWRESERVE` action with flag `4` (use original balance of the account) by explicitly setting `original_balance` to `balance - msg_balance_remaining`.
  - Previously it did not work if storage fee was greater than the original balance.
- Jumps to nested continuations of depth more than 8 consume 1 gas for eact subsequent continuation (this does not affect most of TVM code).