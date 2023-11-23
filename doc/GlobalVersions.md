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