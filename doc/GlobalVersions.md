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
**c7** tuple extended from 14 to 15 elements. The new element is a tuple that contains some config parameters as cell slices.
If the parameter is absent from the config, the value is null.
* **0**: `StoragePrices` from `ConfigParam 18`. Not the whole dict, but only the one StoragePrices entry (one which corresponds to the current time).
* **1**: `ConfigParam 19` (global id).
* **2**: `ConfigParam 20` (mc gas prices).
* **3**: `ConfigParam 21` (gas prices).
* **4**: `ConfigParam 24` (mc fwd fees).
* **5**: `ConfigParam 25` (fwd fees).
* **6**: `ConfigParam 43` (size limits).

### New TVM instructions

#### Fee calculation
* `GETEXECUTIONPRICE` (`gas_used is_mc - price`) - calculates gas fee.
* `GETSTORAGEPRICE` (`cells bits seconds is_mc - price`) - calculates storage fees (only current StoragePrices entry is used).
* `GETFORWARDPRICE` (`cells bits is_mc - price`) - calculates forward fee.
* `GETPRECOMPILEDGAS` (`- null`) - reserved, currently returns `null`.

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