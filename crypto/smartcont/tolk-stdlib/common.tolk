// Standard library for Tolk (LGPL licence).
// It contains common functions that are available out of the box, the user doesn't have to import anything.
// More specific functions are required to be imported explicitly, like "@stdlib/tvm-dicts".
tolk 0.12

/// In Tolk v1.x there would be a type `map<K,V>`.
/// Currently, working with dictionaries is still low-level, with raw cells.
/// But just for clarity, we use "dict" instead of a "cell?" where a cell-dictionary is assumed.
/// Every dictionary object can be null. TVM NULL is essentially "empty dictionary".
type dict = cell?;

/**
    Tuple manipulation primitives.
    Elements of a tuple can be of arbitrary type.
    Note that atomic type `tuple` can't be cast to composite tuple type (e.g. `[int, cell]`) and vise versa.
 */

/// Creates a tuple with zero elements.
@pure
fun createEmptyTuple(): tuple
    asm "NIL";

/// Appends a value to tuple, resulting in `Tuple t' = (x1, ..., xn, value)`.
/// If its size exceeds 255, throws a type check exception.
@pure
fun tuple.push<T>(mutate self, value: T): void
    asm "TPUSH";

/// Returns the first element of a non-empty tuple.
/// `t.0` is actually the same as `t.first()`
@pure
fun tuple.first<T>(self): T
    asm "FIRST";

/// Returns the [`index`]-th element of a tuple.
/// `t.i` is actually the same as `t.get(i)`
@pure
fun tuple.get<T>(self, index: int): T
    builtin;

/// Sets the [`index`]-th element of a tuple to a specified value
/// (element with this index must already exist, a new element isn't created).
/// `t.i = value` is actually the same as `t.set(value, i)`
@pure
fun tuple.set<T>(mutate self, value: T, index: int): void
    builtin;

/// Returns the size of a tuple (elements count in it).
@pure
fun tuple.size(self): int
    asm "TLEN";

/// Returns the last element of a non-empty tuple.
@pure
fun tuple.last<T>(self): T
    asm "LAST";


/**
    Mathematical primitives.
 */

/// Converts a constant floating-point string to nanotoncoins.
/// Example: `ton("0.05")` is equal to 50000000.
/// Note, that `ton()` requires a constant string; `ton(some_var)` is an error
@pure
fun ton(floatString: slice): coins
    builtin;

/// Computes the minimum of two integers.
@pure
fun min(x: int, y: int): int
    asm "MIN";

/// Computes the maximum of two integers.
@pure
fun max(x: int, y: int): int
    asm "MAX";

/// Sorts two integers.
/// Example: `minMax(x, y)` with (x=20, y=10) and with (x=10, y=20) returns (10, 20)
@pure
fun minMax(x: int, y: int): (int, int)
    asm "MINMAX";

/// Computes the absolute value of an integer.
@pure
fun abs(x: int): int
    asm "ABS";

/// Returns the sign of an integer: `-1` if x < 0, `0` if x == 0, `1` if x > 0.
@pure
fun sign(x: int): int
    asm "SGN";

/// Computes the quotient and remainder of [x] / [y]. Example: divMod(112,3) = (37,1)
@pure
fun divMod(x: int, y: int): (int, int)
    asm "DIVMOD";

/// Computes the remainder and quotient of [x] / [y]. Example: modDiv(112,3) = (1,37)
@pure
fun modDiv(x: int, y: int): (int, int)
    asm(-> 1 0) "DIVMOD";

/// Computes multiple-then-divide: floor([x] * [y] / [z]).
/// The intermediate result is stored in a 513-bit integer to prevent precision loss.
@pure
fun mulDivFloor(x: int, y: int, z: int): int
    builtin;

/// Similar to `mulDivFloor`, but rounds the result: round([x] * [y] / [z]).
@pure
fun mulDivRound(x: int, y: int, z: int): int
    builtin;

/// Similar to `mulDivFloor`, but ceils the result: ceil([x] * [y] / [z]).
@pure
fun mulDivCeil(x: int, y: int, z: int): int
    builtin;

/// Computes the quotient and remainder of ([x] * [y] / [z]). Example: mulDivMod(112,3,10) = (33,6)
@pure
fun mulDivMod(x: int, y: int, z: int): (int, int)
    builtin;


/**
    Global getters and setters of current contract state.
 */

/// `contract` is a built-in struct, it has only static methods.
/// Example: `contract.getCode()` and other methods.
struct contract {}

/// Returns the internal address of the current smart contract as a Slice with a `MsgAddressInt`.
/// If necessary, it can be parsed further using primitives such as [parseStandardAddress].
@pure
fun contract.getAddress(): slice
    asm "MYADDR";

/// Returns the balance (in nanotoncoins) of the smart contract at the start of Computation Phase.
/// Note that RAW primitives such as [sendMessage] do not update this field.
@pure
fun contract.getOriginalBalance(): coins
    asm "BALANCE" "FIRST";

/// Same as [contract.getOriginalBalance], but returns a tuple:
/// `int` — balance in nanotoncoins;
/// `dict` — a dictionary with 32-bit keys representing the balance of "extra currencies".
@pure
fun contract.getOriginalBalanceWithExtraCurrencies(): [coins, dict]
    asm "BALANCE";

/// Returns the persistent contract storage cell. It can be parsed or modified with slice and builder primitives later.
@pure
fun contract.getData(): cell
    asm "c4 PUSH";

/// Sets `cell` [c] as persistent contract data. You can update persistent contract storage with this primitive.
fun contract.setData(c: cell): void
    asm "c4 POP";

/// Retrieves code of smart-contract from c7
@pure
fun contract.getCode(): cell
    asm "MYCODE";

/// Creates an output action that would change this smart contract code to that given by cell [newCode].
/// Notice that this change will take effect only after the successful termination of the current run of the smart contract.
fun contract.setCodePostponed(newCode: cell): void
    asm "SETCODE";


/**
    Global getters of current blockchain (environment) state.
*/

const MASTERCHAIN = -1;
const BASECHAIN = 0;

/// `blockchain` is a built-in struct, it has only static methods.
/// Example: `blockchain.configParam(16)` and other methods.
struct blockchain {}

/// Returns current Unix timestamp (in seconds).
@pure
fun blockchain.now(): int
    asm "NOW";

/// Returns the logical time of the current transaction.
@pure
fun blockchain.logicalTime(): int
    asm "LTIME";

/// Returns the starting logical time of the current block.
@pure
fun blockchain.currentBlockLogicalTime(): int
    asm "BLOCKLT";

/// Returns the value of the global configuration parameter with integer index `i` as a `cell` or `null` value.
@pure
fun blockchain.configParam(x: int): cell?
    asm "CONFIGOPTPARAM";

/// Commits the current state of registers `c4` (“persistent data”) and `c5` (“actions”)
/// so that the current execution is considered “successful” with the saved values even if an exception
/// in Computation Phase is thrown later.
fun commitContractDataAndActions(): void
    asm "COMMIT";


/**
    Signature checks, hashing, cryptography.
 */

/// Compile-time function that calculates crc32 of a constant string.
/// Example: `const op = stringCrc32("some_str")` = 4013618352 = 0xEF3AF4B0
/// Note: stringCrc32(slice_var) does not work! It accepts a constant string and works at compile-time.
@pure
fun stringCrc32(constString: slice): int
    builtin;

/// Compile-time function that calculates crc16 (XMODEM) of a constant string.
/// Example: `const op = stringCrc16("some_str")` = 53407 = 0xD09F
/// Note: stringCrc16(slice_var) does not work! It accepts a constant string and works at compile-time.
@pure
fun stringCrc16(constString: slice): int
    builtin;

/// Compile-time function that calculates sha256 of a constant string and returns 256-bit integer.
/// Example: `const hash = stringSha256("some_crypto_key")`
/// Note: it's a compile-time function, `stringSha256(slice_var)` does not work.
/// Use `sliceBitsHash` or `sliceHash` (declared below) to hash a slice without/with its refs at runtime.
@pure
fun stringSha256(constString: slice): int
    builtin;

/// Compile-time function that calculates sha256 of a constant string and takes the first 32 bits.
/// Example: `const minihash = stringSha256_32("some_crypto_key")`
/// Note: stringSha256_32(slice_var) does not work! It accepts a constant string and works at compile-time.
@pure
fun stringSha256_32(constString: slice): int
    builtin;

/// Compile-time function that takes N-chars ascii string and interprets it as a number in base 256.
/// Example: `const value = stringToBase256("AB")` = 16706 (65*256 + 66)
/// Note: stringToBase256(slice_var) does not work! It accepts a constant string and works at compile-time.
@pure
fun stringToBase256(constString: slice): int
    builtin;

/// Computes the representation hash of a `cell` and returns it as a 256-bit unsigned integer `x`.
/// Useful for signing and checking signatures of arbitrary entities represented by a tree of cells.
@pure
fun cell.hash(self): int
    asm "HASHCU";

/// Computes the hash of a `slice` and returns it as a 256-bit unsigned integer `x`.
/// The result is the same as if an ordinary cell containing only data and references from `s` had been created
/// and its hash computed by [cell.hash].
@pure
fun slice.hash(self): int
    asm "HASHSU";

/// Computes sha256 of the data bits of a `slice`. If the bit length of `s` is not divisible by eight,
/// throws a cell underflow exception. The hash value is returned as a 256-bit unsigned integer `x`.
@pure
fun slice.bitsHash(self): int
    asm "SHA256U";

/// Checks the Ed25519-`signature` of a `hash` (a 256-bit unsigned integer, usually computed as the hash of some data)
/// using [publicKey] (also represented by a 256-bit unsigned integer).
/// The signature must contain at least 512 data bits; only the first 512 bits are used.
/// The result is `−1` if the signature is valid, `0` otherwise.
/// Note that `CHKSIGNU` creates a 256-bit slice with the hash and calls `CHKSIGNS`.
/// That is, if [hash] is computed as the hash of some data, these data are hashed twice,
/// the second hashing occurring inside `CHKSIGNS`.
@pure
fun isSignatureValid(hash: int, signature: slice, publicKey: int): bool
    asm "CHKSIGNU";

/// Checks whether [signature] is a valid Ed25519-signature of the data portion of `slice data` using `publicKey`,
/// similarly to [isSignatureValid].
/// If the bit length of [data] is not divisible by eight, throws a cell underflow exception.
/// The verification of Ed25519 signatures is the standard one,
/// with sha256 used to reduce [data] to the 256-bit number that is actually signed.
@pure
fun isSliceSignatureValid(data: slice, signature: slice, publicKey: int): bool
    asm "CHKSIGNS";

/// `random` is a built-in struct, it has only static methods.
/// Example: `random.uint256()` and other methods.
struct random {}

/// Generates a new pseudo-random unsigned 256-bit integer x.
/// Ensure you've called [random.initialize] in advance to make it unpredictable!
fun random.uint256(): uint256
    asm "RANDU256";

/// Generates a new pseudo-random integer z in the range 0..limit−1 (or limit..−1, if upto < 0).
/// More precisely, an unsigned random value x is generated as in random; then z := x * limit / 2^256 is computed.
/// Ensure you've called [random.initialize] in advance to make it unpredictable!
fun random.range(limit: int): int
    asm "RAND";

/// Returns the current random seed used to generate pseudo-random numbers.
@pure
fun random.getSeed(): uint256
    asm "RANDSEED";

/// Sets the random seed to the provided value.
fun random.setSeed(seed: uint256): void
    asm "SETRAND";

/// Initializes (mixes) random seed with the provided value.
fun random.initializeBy(mixSeedWith: uint256): void
    asm "ADDRAND";

/// Initializes random seed with current time to make random generation unpredictable.
/// Typically, you call this function once before calling [random.uint256] / [random.range].
fun random.initialize(): void
    asm "LTIME" "ADDRAND";


/**
    Size computation primitives.
    They may be useful for computing storage fees of user-provided data.
 */

/// Returns `(x, y, z, -1)` or `(null, null, null, 0)`.
/// Recursively computes the count of distinct cells `x`, data bits `y`, and cell references `z`
/// in the DAG rooted at `cell` [c], effectively returning the total storage used by this DAG taking into account
/// the identification of equal cells.
/// The values of `x`, `y`, and `z` are computed by a depth-first traversal of this DAG,
/// with a hash table of visited cell hashes used to prevent visits of already-visited cells.
/// The total count of visited cells `x` cannot exceed non-negative [maxCells];
/// otherwise the computation is aborted before visiting the `(maxCells + 1)`-st cell and
/// a zero flag is returned to indicate failure. If [c] is `null`, returns `x = y = z = 0`.
@pure
fun cell.calculateSize(self, maxCells: int): (int, int, int, bool)
    asm "CDATASIZEQ NULLSWAPIFNOT2 NULLSWAPIFNOT";

/// Similar to [cell.calculateSize], but accepting a `slice` [s] instead of a `cell`.
/// The returned value of `x` does not take into account the cell that contains the `slice` [s] itself;
/// however, the data bits and the cell references of [s] are accounted for in `y` and `z`.
@pure
fun slice.calculateSize(self, maxCells: int): (int, int, int, bool)
    asm "SDATASIZEQ NULLSWAPIFNOT2 NULLSWAPIFNOT";

/// A non-quiet version of [cell.calculateSize] that throws a cell overflow exception (`8`) on failure.
fun cell.calculateSizeStrict(self, maxCells: int): (int, int, int)
    asm "CDATASIZE";

/// A non-quiet version of [cell.calculateSize] that throws a cell overflow exception (`8`) on failure.
fun slice.calculateSizeStrict(self, maxCells: int): (int, int, int)
    asm "SDATASIZE";

/// Returns the depth of a `cell`.
/// If [c] has no references, then return `0`;
/// otherwise the returned value is one plus the maximum of depths of cells referred to from [c].
/// If [c] is a `null` instead of a cell, returns zero.
@pure
fun cell?.depth(self): int
    asm "CDEPTH";

/// Returns the depth of a `slice`.
/// If [s] has no references, then returns `0`;
/// otherwise the returned value is one plus the maximum of depths of cells referred to from [s].
@pure
fun slice.depth(self): int
    asm "SDEPTH";

/// Returns the depth of a `builder`.
/// If no cell references are stored in [b], then returns 0;
/// otherwise the returned value is one plus the maximum of depths of cells referred to from [b].
@pure
fun builder.depth(self): int
    asm "BDEPTH";


/**
    Debug primitives.
    Only works for local TVM execution with debug level verbosity.
 */

/// `debug` is a built-in struct, it has only static methods.
/// Example: `debug.print(v)` and other methods.
struct debug {}

/// Dump a variable [x] to the debug log.
fun debug.print<T>(x: T): void
    builtin;

/// Dump a string [x] to the debug log.
fun debug.printString<T>(x: T): void
    builtin;

/// Dumps the stack (at most the top 255 values) and shows the total stack depth.
fun debug.dumpStack(): void
    builtin;


/**
    Slice primitives: parsing cells.
    When you _load_ some data, you mutate the slice (shifting an internal pointer on the stack).
    When you _preload_ some data, you just get the result without mutating the slice.
 */

/// Compile-time function that converts a constant string to TL-encoded MsgAddressInt (TVM slice).
/// Example: stringAddressToSlice("EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5")
/// Example: stringAddressToSlice("0:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8")
/// Note: it's a compile-time function, `stringAddressToSlice(slice_var)` does not work.
/// Use `parseStandardAddress` to decode a slice at runtime into workchain and hash.
@pure
fun stringAddressToSlice(constStringAddress: slice): slice
    builtin;

/// Compile-time function that converts a constant hex-encoded string to N/2 bytes.
/// Example: `const v = stringHexToSlice("abcdef")` = slice with 3 bytes `[ 0xAB, 0xCD, 0xEF ]`
/// Note: stringHexToSlice(slice_var) does not work! It accepts a constant string and works at compile-time.
@pure
fun stringHexToSlice(constStringBytesHex: slice): slice
    builtin;

/// Converts a `cell` into a `slice`. Notice that [c] must be either an ordinary cell,
/// or an exotic cell (see [TVM.pdf](https://ton-blockchain.github.io/docs/tvm.pdf), 3.1.2)
/// which is automatically loaded to yield an ordinary cell `c'`, converted into a `slice` afterwards.
@pure
fun cell.beginParse(self): slice
    asm "CTOS";

/// Checks if slice is empty. If not, throws an exception.
fun slice.assertEnd(self): void
    asm "ENDS";

/// Loads the next reference from the slice.
@pure
fun slice.loadRef(mutate self): cell
    asm( -> 1 0) "LDREF";

/// Preloads the next reference from the slice.
@pure
fun slice.preloadRef(self): cell
    asm "PLDREF";

/// Loads a signed [len]-bit integer from a slice.
@pure
fun slice.loadInt(mutate self, len: int): int
    builtin;

/// Loads an unsigned [len]-bit integer from a slice.
@pure
fun slice.loadUint(mutate self, len: int): int
    builtin;

/// Loads the first `0 ≤ len ≤ 1023` bits from slice [s] into a separate slice `s''`.
@pure
fun slice.loadBits(mutate self, len: int): slice
    builtin;

/// Preloads a signed [len]-bit integer from a slice.
@pure
fun slice.preloadInt(self, len: int): int
    builtin;

/// Preloads an unsigned [len]-bit integer from a slice.
@pure
fun slice.preloadUint(self, len: int): int
    builtin;

/// Preloads the first `0 ≤ len ≤ 1023` bits from slice [s] into a separate slice.
@pure
fun slice.preloadBits(self, len: int): slice
    builtin;

/// Loads serialized amount of Toncoins (any unsigned integer up to `2^120 - 1`).
@pure
fun slice.loadCoins(mutate self): coins
    asm( -> 1 0) "LDGRAMS";

/// Loads bool (-1 or 0) from a slice
@pure
fun slice.loadBool(mutate self): bool
    asm( -> 1 0) "1 LDI";

/// Shifts a slice pointer to [len] bits forward, mutating the slice.
@pure
fun slice.skipBits(mutate self, len: int): self
    asm "SDSKIPFIRST";

/// Returns the first `0 ≤ len ≤ 1023` bits of a slice.
@pure
fun slice.getFirstBits(self, len: int): slice
    asm "SDCUTFIRST";

/// Returns all but the last `0 ≤ len ≤ 1023` bits of a slice.
@pure
fun slice.removeLastBits(mutate self, len: int): self
    asm "SDSKIPLAST";

/// Returns the last `0 ≤ len ≤ 1023` bits of a slice.
@pure
fun slice.getLastBits(self, len: int): slice
    asm "SDCUTLAST";

/// Loads a dictionary (TL HashMapE structure, represented as TVM cell) from a slice.
/// Returns `null` if `nothing` constructor is used.
@pure
fun slice.loadDict(mutate self): dict
    asm( -> 1 0) "LDDICT";

/// Preloads a dictionary (cell) from a slice.
@pure
fun slice.preloadDict(self): dict
    asm "PLDDICT";

/// Loads a dictionary as [slice.loadDict], but returns only the remainder of the slice.
@pure
fun slice.skipDict(mutate self): self
    asm "SKIPDICT";

/// Loads (Maybe ^Cell) from a slice.
/// In other words, loads 1 bit: if it's true, loads the first ref, otherwise returns `null`.
@pure
fun slice.loadMaybeRef(mutate self): cell?
    asm( -> 1 0) "LDOPTREF";

/// Preloads (Maybe ^Cell) from a slice.
@pure
fun slice.preloadMaybeRef(self): cell?
    asm "PLDOPTREF";

/// Loads (Maybe ^Cell), but returns only the remainder of the slice.
@pure
fun slice.skipMaybeRef(mutate self): self
    asm "SKIPOPTREF";

/**
    Builder primitives: constructing cells.
    When you _store_ some data, you mutate the builder (shifting an internal pointer on the stack).
    All the primitives below first check whether there is enough space in the `builder`,
    and only then check the range of the value being serialized.
 */

/// Creates a new empty builder.
@pure
fun beginCell(): builder
    asm "NEWC";

/// Converts a builder into an ordinary `cell`.
@pure
fun builder.endCell(self): cell
    asm "ENDC";

/// Stores a reference to a cell into a builder.
@pure
fun builder.storeRef(mutate self, c: cell): self
    asm(c self) "STREF";

/// Stores a signed [len]-bit integer into a builder (`0 ≤ len ≤ 257`).
@pure
fun builder.storeInt(mutate self, x: int, len: int): self
    builtin;

/// Stores an unsigned [len]-bit integer into a builder (`0 ≤ len ≤ 256`).
@pure
fun builder.storeUint(mutate self, x: int, len: int): self
    builtin;

/// Stores a slice into a builder.
@pure
fun builder.storeSlice(mutate self, s: slice): self
    asm "STSLICER";

/// Stores amount of Toncoins into a builder.
@pure
fun builder.storeCoins(mutate self, x: coins): self
    asm "STGRAMS";

/// Stores bool (-1 or 0) into a builder.
/// Attention: true value is `-1`, not 1! If you pass `1` here, TVM will throw an exception.
@pure
fun builder.storeBool(mutate self, x: bool): self
    asm(x self) "1 STI";

/// Stores dictionary (represented by TVM `cell` or `null`) into a builder.
/// In other words, stores a `1`-bit and a reference to [c] if [c] is not `null` and `0`-bit otherwise.
@pure
fun builder.storeDict(mutate self, c: dict): self
    asm(c self) "STDICT";

/// Stores (Maybe ^Cell) into a builder.
/// In other words, if cell is `null`, store '0' bit; otherwise, store '1' and a ref to [c].
@pure
fun builder.storeMaybeRef(mutate self, c: cell?): self
    asm(c self) "STOPTREF";

/// Concatenates two builders.
@pure
fun builder.storeBuilder(mutate self, from: builder): self
    asm "STBR";

/// Stores a slice representing TL addr_none$00 (two `0` bits).
@pure
fun builder.storeAddressNone(mutate self): self
    asm "b{00} STSLICECONST";


/**
    Slice size primitives.
 */

/// Returns the number of references in a slice.
@pure
fun slice.remainingRefsCount(self): int
    asm "SREFS";

/// Returns the number of data bits in a slice.
@pure
fun slice.remainingBitsCount(self): int
    asm "SBITS";

/// Returns both the number of data bits and the number of references in a slice.
@pure
fun slice.remainingBitsAndRefsCount(self): (int, int)
    asm "SBITREFS";

/// Checks whether a slice is empty (i.e., contains no bits of data and no cell references).
@pure
fun slice.isEnd(self): bool
    asm "SEMPTY";

/// Checks whether a slice has no bits of data.
@pure
fun slice.isEndOfBits(self): bool
    asm "SDEMPTY";

/// Checks whether a slice has no references.
@pure
fun slice.isEndOfRefs(self): bool
    asm "SREMPTY";

/// Checks whether data parts of two slices coinside.
@pure
fun slice.bitsEqual(self, b: slice): bool
    asm "SDEQ";

/// Returns the number of cell references already stored in a builder.
@pure
fun builder.refsCount(self): int
    asm "BREFS";

/// Returns the number of data bits already stored in a builder.
@pure
fun builder.bitsCount(self): int
    asm "BBITS";


/**
    Address manipulation primitives.
    The address manipulation primitives listed below serialize and deserialize values according to the following TL-B scheme:
  ```TL-B
  addr_none$00 = MsgAddressExt;
  addr_extern$01 len:(## 8) external_address:(bits len)
               = MsgAddressExt;
  anycast_info$_ depth:(#<= 30) { depth >= 1 }
    rewrite_pfx:(bits depth) = Anycast;
  addr_std$10 anycast:(Maybe Anycast)
    workchain_id:int8 address:bits256 = MsgAddressInt;
  addr_var$11 anycast:(Maybe Anycast) addr_len:(## 9)
    workchain_id:int32 address:(bits addr_len) = MsgAddressInt;
  _ _:MsgAddressInt = MsgAddress;
  _ _:MsgAddressExt = MsgAddress;

  int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool
    src:MsgAddress dest:MsgAddressInt
    value:CurrencyCollection ihr_fee:Grams fwd_fee:Grams
    created_lt:uint64 created_at:uint32 = CommonMsgInfoRelaxed;
  ext_out_msg_info$11 src:MsgAddress dest:MsgAddressExt
    created_lt:uint64 created_at:uint32 = CommonMsgInfoRelaxed;
  ```
  A deserialized `MsgAddress` is represented by a tuple `t` as follows:

  - `addr_none` is represented by `t = (0)`,
    i.e., a tuple containing exactly one integer equal to zero.
  - `addr_extern` is represented by `t = (1, s)`,
    where slice `s` contains the field `external_address`. In other words, `
    t` is a pair (a tuple consisting of two entries), containing an integer equal to one and slice `s`.
  - `addr_std` is represented by `t = (2, u, x, s)`,
    where `u` is either a `null` (if `anycast` is absent) or a slice `s'` containing `rewrite_pfx` (if anycast is present).
    Next, integer `x` is the `workchain_id`, and slice `s` contains the address.
  - `addr_var` is represented by `t = (3, u, x, s)`,
    where `u`, `x`, and `s` have the same meaning as for `addr_std`.
 */

/// Loads from slice [s] the only prefix that is a valid `MsgAddress`,
/// and returns both this prefix `s'` and the remainder `s''` of [s] as slices.
@pure
fun slice.loadAddress(mutate self): slice
    asm( -> 1 0) "LDMSGADDR";

/// Decomposes slice [s] containing a valid `MsgAddress` into a `tuple t` with separate fields of this `MsgAddress`.
/// If [s] is not a valid `MsgAddress`, a cell deserialization exception is thrown.
@pure
fun parseAddress(s: slice): tuple
    asm "PARSEMSGADDR";

/// Parses slice [s] containing a valid `MsgAddressInt` (usually a `msg_addr_std`),
/// applies rewriting from the anycast (if present) to the same-length prefix of the address,
/// and returns both the workchain and the 256-bit address as integers.
/// If the address is not 256-bit, or if [s] is not a valid serialization of `MsgAddressInt`,
/// throws a cell deserialization exception.
@pure
fun parseStandardAddress(s: slice): (int, int)
    asm "REWRITESTDADDR";

/// Creates a slice representing TL addr_none$00 (two `0` bits).
@pure
fun createAddressNone(): slice
    asm "b{00} PUSHSLICE";

/// Returns if a slice pointer contains an empty address.
/// In other words, a slice starts with two `0` bits (TL addr_none$00).
@pure
fun addressIsNone(s: slice): bool
    asm "2 PLDU" "0 EQINT";


/**
    Reserving Toncoins on balance and its flags.
 */

/// mode = 0: Reserve exact amount of nanotoncoins
const RESERVE_MODE_EXACT_AMOUNT = 0;
/// +1: Actually reserves all but amount, meaning `currentContractBalance - amount`
const RESERVE_MODE_ALL_BUT_AMOUNT = 1;
/// +2: Actually set `min(amount, currentContractBalance)` (without this mode, if amount is greater, the action will fail)
const RESERVE_MODE_AT_MOST = 2;
/// +4: [amount] is increased by the _original_ balance of the current account (before the compute phase).
const RESERVE_MODE_INCREASE_BY_ORIGINAL_BALANCE = 4;
/// +8: Actually sets `amount = -amount` before performing any further actions.
const RESERVE_MODE_NEGATE_AMOUNT = 8;
/// +16: If this action fails, the transaction will be bounced.
const RESERVE_MODE_BOUNCE_ON_ACTION_FAIL = 16;

/// Creates an output action which would reserve Toncoins on balance.
/// For [reserveMode] consider constants above.
fun reserveToncoinsOnBalance(nanoTonCoins: coins, reserveMode: int): void
    asm "RAWRESERVE";

/// Similar to [reserveToncoinsOnBalance], but also accepts a dictionary extraAmount (represented by a cell or null)
/// with extra currencies. In this way currencies other than Toncoin can be reserved.
fun reserveExtraCurrenciesOnBalance(nanoTonCoins: coins, extraAmount: dict, reserveMode: int): void
    asm "RAWRESERVEX";


/**
  Messages sending and parsing primitives.
  Working with messages is low-level right now, but still, every contract should do that.

  `Message` structure, its header and so on are specified in TL-B scheme, particularly:
  int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool ... = CommonMsgInfo;
 */

/// 0b011000 tag - 0, ihr_disabled - 1, bounce - 1, bounced - 0, src = adr_none$00
const BOUNCEABLE = 0x18;
/// 0b010000 tag - 0, ihr_disabled - 1, bounce - 0, bounced - 0, src = adr_none$00
const NON_BOUNCEABLE = 0x10;

/// Load msgFlags from incoming message body (4 bits).
@pure
fun slice.loadMessageFlags(mutate self): int
    asm( -> 1 0) "4 LDU";

/// Having msgFlags (4 bits), check that a message is bounced.
/// Effectively, it's `msgFlags & 1` (the lowest bit present).
@pure
fun isMessageBounced(msgFlags: int): bool
    asm "2 PUSHINT" "MODR";

/// Skip 0xFFFFFFFF prefix (when a message is bounced).
@pure
fun slice.skipBouncedPrefix(mutate self): self
    asm "32 PUSHINT" "SDSKIPFIRST";

/// The guideline recommends to start the body of an internal message with uint32 `op` and uint64 `queryId`.
@pure
fun slice.loadMessageOp(mutate self): int
    asm( -> 1 0) "32 LDU";

@pure
fun slice.skipMessageOp(mutate self): self
    asm "32 PUSHINT" "SDSKIPFIRST";

@pure
fun builder.storeMessageOp(mutate self, op: int): self
    asm(op self) "32 STU";

/// The guideline recommends that uint64 `queryId` should follow uint32 `op`.
@pure
fun slice.loadMessageQueryId(mutate self): int
    asm( -> 1 0) "64 LDU";

@pure
fun slice.skipMessageQueryId(mutate self): self
    asm "64 PUSHINT" "SDSKIPFIRST";

@pure
fun builder.storeMessageQueryId(mutate self, queryId: int): self
    asm(queryId self) "64 STU";

/// SEND MODES - https://docs.ton.org/tvm.pdf page 137, SENDRAWMSG

/// mode = 0 is used for ordinary messages; the gas fees are deducted from the senging amount; action phaes should NOT be ignored.
const SEND_MODE_REGULAR = 0;
/// +1 means that the sender wants to pay transfer fees separately.
const SEND_MODE_PAY_FEES_SEPARATELY = 1;
/// +2 means that any errors arising while processing this message during the action phase should be ignored.
const SEND_MODE_IGNORE_ERRORS = 2;
/// in the case of action fail - bounce transaction. No effect if SEND_MODE_IGNORE_ERRORS (+2) is used. TVM UPGRADE 2023-07. https://docs.ton.org/learn/tvm-instructions/tvm-upgrade-2023-07#sending-messages
const SEND_MODE_BOUNCE_ON_ACTION_FAIL = 16;
/// mode = 32 means that the current account must be destroyed if its resulting balance is zero.
const SEND_MODE_DESTROY = 32;
/// mode = 64 is used for messages that carry all the remaining value of the inbound message in addition to the value initially indicated in the new message.
const SEND_MODE_CARRY_ALL_REMAINING_MESSAGE_VALUE = 64;
/// mode = 128 is used for messages that are to carry all the remaining balance of the current smart contract (instead of the value originally indicated in the message).
const SEND_MODE_CARRY_ALL_BALANCE = 128;
/// do not create an action, only estimate fee. TVM UPGRADE 2023-07. https://docs.ton.org/learn/tvm-instructions/tvm-upgrade-2023-07#sending-messages
const SEND_MODE_ESTIMATE_FEE_ONLY = 1024;
/// Other modes affect the fee calculation as follows:
/// +64 substitutes the entire balance of the incoming message as an outcoming value (slightly inaccurate, gas expenses that cannot be estimated before the computation is completed are not taken into account).
/// +128 substitutes the value of the entire balance of the contract before the start of the computation phase (slightly inaccurate, since gas expenses that cannot be estimated before the completion of the computation phase are not taken into account).

/// Sends a raw message — a correctly serialized TL object `Message X`.
/// For `mode`, see constants above (except SEND_MODE_ESTIMATE_FEE_ONLY).
fun sendRawMessage(msg: cell, mode: int): void
    asm "SENDRAWMSG";

/// Creates an output action and returns a fee for creating a message.
/// Mode has the same effect as in the case of SENDRAWMSG.
/// For mode including SEND_MODE_ESTIMATE_FEE_ONLY it just returns estimated fee without sending a message.
fun sendMessage(msg: cell, mode: int): int
    asm "SENDMSG";
