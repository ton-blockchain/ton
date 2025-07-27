// Standard library for Tolk (LGPL licence).
// It contains common functions that are available out of the box, the user doesn't have to import anything.
// More specific functions are required to be imported explicitly, like "@stdlib/tvm-dicts".
tolk 1.0

/// In Tolk v1.x there would be a type `map<K,V>`.
/// Currently, working with dictionaries is still low-level, with raw cells.
/// But just for clarity, we use "dict" instead of a "cell?" where a cell-dictionary is assumed.
/// Every dictionary object can be null. TVM NULL is essentially "empty dictionary".
type dict = cell?

/**
    Tuple manipulation primitives.
    Elements of a tuple can be of arbitrary type.
    Note that atomic type `tuple` can't be cast to composite tuple type (e.g. `[int, cell]`) and vise versa.
 */

/// Creates a tuple with zero elements.
@pure
fun createEmptyTuple(): tuple
    asm "NIL"

/// Appends a value to tuple, resulting in `Tuple t' = (x1, ..., xn, value)`.
/// If its size exceeds 255, throws a type check exception.
@pure
fun tuple.push<T>(mutate self, value: T): void
    asm "TPUSH"

/// Returns the first element of a non-empty tuple.
/// `t.0` is actually the same as `t.first()`
@pure
fun tuple.first<T>(self): T
    asm "FIRST"

/// Returns the [`index`]-th element of a tuple.
/// `t.i` is actually the same as `t.get(i)`
@pure
fun tuple.get<T>(self, index: int): T
    builtin

/// Sets the [`index`]-th element of a tuple to a specified value
/// (element with this index must already exist, a new element isn't created).
/// `t.i = value` is actually the same as `t.set(value, i)`
@pure
fun tuple.set<T>(mutate self, value: T, index: int): void
    builtin

/// Returns the size of a tuple (elements count in it).
@pure
fun tuple.size(self): int
    asm "TLEN"

/// Returns the last element of a non-empty tuple.
@pure
fun tuple.last<T>(self): T
    asm "LAST"

/// Pops and returns the last element of a non-empty tuple.
@pure
fun tuple.pop<T>(mutate self): T
    asm "TPOP"


/**
    Mathematical primitives.
 */

/// Converts a constant floating-point string to nanotoncoins.
/// Example: `ton("0.05")` is equal to 50000000.
/// Note, that `ton()` requires a constant string; `ton(some_var)` is an error
@pure
fun ton(floatString: slice): coins
    builtin

/// Computes the minimum of two integers.
@pure
fun min(x: int, y: int): int
    asm "MIN"

/// Computes the maximum of two integers.
@pure
fun max(x: int, y: int): int
    asm "MAX"

/// Sorts two integers.
/// Example: `minMax(x, y)` with (x=20, y=10) and with (x=10, y=20) returns (10, 20)
@pure
fun minMax(x: int, y: int): (int, int)
    asm "MINMAX"

/// Computes the absolute value of an integer.
@pure
fun abs(x: int): int
    asm "ABS"

/// Returns the sign of an integer: `-1` if x < 0, `0` if x == 0, `1` if x > 0.
@pure
fun sign(x: int): int
    asm "SGN"

/// Computes the quotient and remainder of [x] / [y]. Example: divMod(112,3) = (37,1)
@pure
fun divMod(x: int, y: int): (int, int)
    asm "DIVMOD"

/// Computes the remainder and quotient of [x] / [y]. Example: modDiv(112,3) = (1,37)
@pure
fun modDiv(x: int, y: int): (int, int)
    asm(-> 1 0) "DIVMOD"

/// Computes multiple-then-divide: floor([x] * [y] / [z]).
/// The intermediate result is stored in a 513-bit integer to prevent precision loss.
@pure
fun mulDivFloor(x: int, y: int, z: int): int
    builtin

/// Similar to `mulDivFloor`, but rounds the result: round([x] * [y] / [z]).
@pure
fun mulDivRound(x: int, y: int, z: int): int
    builtin

/// Similar to `mulDivFloor`, but ceils the result: ceil([x] * [y] / [z]).
@pure
fun mulDivCeil(x: int, y: int, z: int): int
    builtin

/// Computes the quotient and remainder of ([x] * [y] / [z]). Example: mulDivMod(112,3,10) = (33,6)
@pure
fun mulDivMod(x: int, y: int, z: int): (int, int)
    builtin


/**
    Global getters and setters of current contract state.
 */

/// `contract` is a built-in struct, it has only static methods.
/// Example: `contract.getCode()` and other methods.
struct contract

/// Returns the internal address of the current smart contract.
/// If necessary, it can be parsed further using [address.getWorkchain] and others.
@pure
fun contract.getAddress(): address
    asm "MYADDR"

/// Returns the balance (in nanotoncoins) of the smart contract at the start of Computation Phase.
/// Note that RAW primitives such as [sendMessage] do not update this field.
@pure
fun contract.getOriginalBalance(): coins
    asm "BALANCE" "FIRST"

/// Same as [contract.getOriginalBalance], but returns a tuple:
/// `int` — balance in nanotoncoins;
/// `dict` — a dictionary with 32-bit keys representing the balance of "extra currencies".
@pure
fun contract.getOriginalBalanceWithExtraCurrencies(): [coins, dict]
    asm "BALANCE"

/// Returns the persistent contract storage cell. It can be parsed or modified with slice and builder primitives later.
@pure
fun contract.getData(): cell
    asm "c4 PUSH"

/// Sets `cell` [c] as persistent contract data. You can update persistent contract storage with this primitive.
fun contract.setData(c: cell): void
    asm "c4 POP"

/// Retrieves code of smart-contract from c7
@pure
fun contract.getCode(): cell
    asm "MYCODE"

/// Creates an output action that would change this smart contract code to that given by cell [newCode].
/// Notice that this change will take effect only after the successful termination of the current run of the smart contract.
fun contract.setCodePostponed(newCode: cell): void
    asm "SETCODE"


/**
    Global getters of current blockchain (environment) state.
*/

const MASTERCHAIN = -1
const BASECHAIN = 0

/// `blockchain` is a built-in struct, it has only static methods.
/// Example: `blockchain.configParam(16)` and other methods.
struct blockchain

/// Returns current Unix timestamp (in seconds).
@pure
fun blockchain.now(): int
    asm "NOW"

/// Returns the logical time of the current transaction.
@pure
fun blockchain.logicalTime(): int
    asm "LTIME"

/// Returns the starting logical time of the current block.
@pure
fun blockchain.currentBlockLogicalTime(): int
    asm "BLOCKLT"

/// Returns the value of the global configuration parameter with integer index `i` as a `cell` or `null` value.
@pure
fun blockchain.configParam(x: int): cell?
    asm "CONFIGOPTPARAM"

/// Commits the current state of registers `c4` (“persistent data”) and `c5` (“actions”)
/// so that the current execution is considered “successful” with the saved values even if an exception
/// in Computation Phase is thrown later.
fun commitContractDataAndActions(): void
    asm "COMMIT"


/**
    Auto packing structures to/from cells.
*/

/// PackOptions allows you to control behavior of `obj.toCell()` and similar functions.
struct PackOptions {
    /// when a struct has a field of type `bits128` and similar (it's a slice under the hood),
    /// by default, compiler inserts runtime checks (get bits/refs count + compare with 128 + compare with 0);
    /// these checks ensure that serialized binary data will be correct, but they cost gas;
    /// however, if you guarantee that a slice is valid (for example, it comes from trusted sources),
    /// set this option to true to disable runtime checks;
    /// note: `int32` and other are always validated for overflow without any extra gas,
    /// so this flag controls only rarely used `bitsN` type
    skipBitsNValidation: bool = false,
}

/// UnpackOptions allows you to control behavior of `MyStruct.fromCell(c)` and similar functions.
struct UnpackOptions {
    /// after finished reading all fields from a cell/slice, call [slice.assertEnd] to ensure no remaining data left;
    /// it's the default behavior, it ensures that you've fully described data you're reading with a struct;
    /// example: `struct Point { x: int8; y: int8 }`, input "0102" is ok, "0102FF" will throw excno 9;
    /// note: setting this to false does not decrease gas (DROP from a stack and ENDS cost the same);
    /// note: this option controls [T.fromCell] and [T.fromSlice], but is ignored by [slice.loadAny];
    /// note: `lazy` ignores this option, because it reads fields on demand or even skips them
    assertEndAfterReading: bool = true,

    /// this excNo is thrown if a prefix doesn't match, e.g. for `struct (0x01) A` given input "88...";
    /// similarly, for a union type, this is thrown when none of the opcodes match;
    /// note: `lazy` ignores this option if you have `else` in `match` (you write custom logic there)
    throwIfOpcodeDoesNotMatch: int = 63,
}

/// Convert anything to a cell (most likely, you'll call it for structures).
/// Example:
/// ```
/// var st: MyStorage = { ... };
/// contract.setData(st.toCell());
/// ```
/// Internally, a builder is created, all fields are serialized one by one, and a builder is flushed
/// (beginCell() + serialize fields + endCell()).
@pure
fun T.toCell(self, options: PackOptions = {}): Cell<T>
    builtin

/// Parse anything from a cell (most likely, you'll call it for structures).
/// Example:
/// ```
/// var st = MyStorage.fromCell(contract.getData());
/// ```
/// Internally, a cell is unpacked to a slice, and that slice is parsed
/// (packedCell.beginParse() + read from slice).
@pure
fun T.fromCell(packedCell: cell, options: UnpackOptions = {}): T
    builtin

/// Parse anything from a slice (most likely, you'll call it for structures).
/// Example:
/// ```
/// var msg = CounterIncrement.fromSlice(cs);
/// ```
/// All fields are read from a slice immediately.
/// If a slice is corrupted, an exception is thrown (most likely, excode 9 "cell underflow").
/// Note, that a passed slice is NOT mutated, its internal pointer is NOT shifted.
/// If you need to mutate it, like `cs.loadInt()`, consider calling `cs.loadAny<CounterIncrement>()`.
@pure
fun T.fromSlice(rawSlice: slice, options: UnpackOptions = {}): T
    builtin

/// Parse anything from a slice, shifting its internal pointer.
/// Similar to `slice.loadUint()` and others, but allows loading structures.
/// Example:
/// ```
/// var st: MyStorage = cs.loadAny();     // or cs.loadAny<MyStorage>()
/// ```
/// Similar to `MyStorage.fromSlice(cs)`, but called as a slice method and mutates the slice.
/// Note: [options.assertEndAfterReading] is ignored by this function, because it's actually intended
/// to read data from the middle.
@pure
fun slice.loadAny<T>(mutate self, options: UnpackOptions = {}): T
    builtin

/// Skip anything in a slice, shifting its internal pointer.
/// Similar to `slice.skipBits()` and others, but allows skipping structures.
/// Example:
/// ```
/// struct TwoInts { a: int32; b: int32; }
/// cs.skipAny<TwoInts>();    // skips 64 bits
/// ```
@pure
fun slice.skipAny<T>(mutate self, options: UnpackOptions = {}): self
    builtin

/// Store anything to a builder.
/// Similar to `builder.storeUint()` and others, but allows storing structures.
/// Example:
/// ```
/// var b = beginCell().storeUint(32).storeAny(msgBody).endCell();
/// ```
@pure
fun builder.storeAny<T>(mutate self, v: T, options: PackOptions = {}): self
    builtin

/// Returns serialization prefix of a struct. Works at compile-time.
/// Example: for `struct (0xF0) AssetRegular { ... }` will return `240`.
@pure
fun T.getDeclaredPackPrefix(): int
    builtin

/// Returns serialization prefix length of a struct. Works at compile-time.
/// Example: for `struct (0xF0) AssetRegular { ... }` will return `16`.
@pure
fun T.getDeclaredPackPrefixLen(): int
    builtin

/// Forces an object created by `lazy` to load fully. Returns the remaining slice (having read all fields).
/// Since `options.assertEndAfterReading` is ignored by `lazy` (fields are loaded on demand),
/// this method can help you overcome this, if you really need to check input consistency.
/// Example:
/// ```
/// val msg = lazy CounterMessage.fromSlice(s);
/// match (msg) {     // it's a lazy match, without creating a union on the stack
///     CounterIncrement => {
///         ...
///         newCounter = curCounter + msg.incBy;   // `incBy` loaded here, on demand
///         msg.forceLoadLazyObject().assertEnd()  // the purpose: get remainder
///     }
/// }
/// ```
/// Note: while [slice.assertEnd] may seem reasonable, these checks are avoided in practice,
/// because the purpose of `lazy` is to auto-detect and load only necessary fields, not up to the end.
@pure
fun T.forceLoadLazyObject(self): slice
    builtin

/// Cell<T> represents a typed cell reference (as opposed to untyped `cell`).
/// Example:
/// ```
/// struct ExtraData { ... }
///
/// struct MyStorage {
///     ...
///     extra:    Cell<ExtraData>;    // TL-B `^ExtraData`
///     optional: Cell<ExtraData>?;   // TL-B `(Maybe ^ExtraData)`
///     code:     cell;               // TL-B `^Cell`
///     data:     cell?;              // TL-B `(Maybe ^Cell)`
/// }
/// ```
/// Note, that `st = MyStorage.fromSlice(s)` does NOT deep-load any refs; `st.extra` is `Cell<T>`, not `T`;
/// you should manually call `st.extra.load()` to get T (ExtraData in this example).
struct Cell<T> {
    tvmCell: cell
}

/// Parse data from already loaded cell reference.
/// Example:
/// ```
/// struct MyStorage { ... extra: Cell<ExtraData>; }
///
/// var st = MyStorage.fromCell(contract.getData());
/// // st.extra is cell; if we need to unpack it, we do
/// var extra = st.extra.load();   // it's ExtraData, unpacked from loaded ref
/// ```
@pure
fun Cell<T>.load(self, options: UnpackOptions = {}): T
    builtin

/// Converts a typed cell into a slice.
@pure
fun Cell<T>.beginParse(self): slice
    asm "CTOS"

/// Returns hash of a typed cell, same as [cell.hash].
@pure
fun Cell<T>.hash(self): uint256
    asm "HASHCU"

/// RemainingBitsAndRefs is a special built-in type to get "all the rest" slice tail on reading.
/// Example:
/// ```
/// struct JettonMessage {
///     ... some fields
///     forwardPayload: RemainingBitsAndRefs;
/// }
/// ```
/// When you deserialize JettonMessage, forwardPayload contains "everything left after reading fields above".
type RemainingBitsAndRefs = slice

/// Creates a cell with zero bits and references.
/// Equivalent to `beginCell().endCell()` but cheaper.
@pure
fun createEmptyCell(): cell
    asm "<b b> PUSHREF"

/// Creates a slice with zero remaining bits and references.
/// Equivalent to `beginCell().endCell().beginParse()` but cheaper.
@pure
fun createEmptySlice(): slice
    asm "x{} PUSHSLICE"


/**
    Signature checks, hashing, cryptography.
 */

/// Compile-time function that calculates crc32 of a constant string.
/// Example: `const op = stringCrc32("some_str")` = 4013618352 = 0xEF3AF4B0
/// Note: stringCrc32(slice_var) does not work! It accepts a constant string and works at compile-time.
@pure
fun stringCrc32(constString: slice): int
    builtin

/// Compile-time function that calculates crc16 (XMODEM) of a constant string.
/// Example: `const op = stringCrc16("some_str")` = 53407 = 0xD09F
/// Note: stringCrc16(slice_var) does not work! It accepts a constant string and works at compile-time.
@pure
fun stringCrc16(constString: slice): int
    builtin

/// Compile-time function that calculates sha256 of a constant string and returns 256-bit integer.
/// Example: `const hash = stringSha256("some_crypto_key")`
/// Note: it's a compile-time function, `stringSha256(slice_var)` does not work.
/// Use `sliceBitsHash` or `sliceHash` (declared below) to hash a slice without/with its refs at runtime.
@pure
fun stringSha256(constString: slice): int
    builtin

/// Compile-time function that calculates sha256 of a constant string and takes the first 32 bits.
/// Example: `const minihash = stringSha256_32("some_crypto_key")`
/// Note: stringSha256_32(slice_var) does not work! It accepts a constant string and works at compile-time.
@pure
fun stringSha256_32(constString: slice): int
    builtin

/// Compile-time function that takes N-chars ascii string and interprets it as a number in base 256.
/// Example: `const value = stringToBase256("AB")` = 16706 (65*256 + 66)
/// Note: stringToBase256(slice_var) does not work! It accepts a constant string and works at compile-time.
@pure
fun stringToBase256(constString: slice): int
    builtin

/// Computes the representation hash of a `cell` and returns it as a 256-bit unsigned integer `x`.
/// Useful for signing and checking signatures of arbitrary entities represented by a tree of cells.
@pure
fun cell.hash(self): uint256
    asm "HASHCU"

/// Computes the hash of a `slice` and returns it as a 256-bit unsigned integer `x`.
/// The result is the same as if an ordinary cell containing only data and references from `s` had been created
/// and its hash computed by [cell.hash].
@pure
fun slice.hash(self): uint256
    asm "HASHSU"

/// Computes sha256 of the data bits of a `slice`. If the bit length of `s` is not divisible by eight,
/// throws a cell underflow exception. The hash value is returned as a 256-bit unsigned integer `x`.
@pure
fun slice.bitsHash(self): uint256
    asm "SHA256U"

/// Checks the Ed25519-`signature` of a `hash` (a 256-bit unsigned integer, usually computed as the hash of some data)
/// using [publicKey] (also represented by a 256-bit unsigned integer).
/// The signature must contain at least 512 data bits; only the first 512 bits are used.
/// The result is `−1` if the signature is valid, `0` otherwise.
/// Note that `CHKSIGNU` creates a 256-bit slice with the hash and calls `CHKSIGNS`.
/// That is, if [hash] is computed as the hash of some data, these data are hashed twice,
/// the second hashing occurring inside `CHKSIGNS`.
@pure
fun isSignatureValid(hash: int, signature: slice, publicKey: int): bool
    asm "CHKSIGNU"

/// Checks whether [signature] is a valid Ed25519-signature of the data portion of `slice data` using `publicKey`,
/// similarly to [isSignatureValid].
/// If the bit length of [data] is not divisible by eight, throws a cell underflow exception.
/// The verification of Ed25519 signatures is the standard one,
/// with sha256 used to reduce [data] to the 256-bit number that is actually signed.
@pure
fun isSliceSignatureValid(data: slice, signature: slice, publicKey: int): bool
    asm "CHKSIGNS"

/// `random` is a built-in struct, it has only static methods.
/// Example: `random.uint256()` and other methods.
struct random

/// Generates a new pseudo-random unsigned 256-bit integer x.
/// Ensure you've called [random.initialize] in advance to make it unpredictable!
fun random.uint256(): uint256
    asm "RANDU256"

/// Generates a new pseudo-random integer z in the range 0..limit−1 (or limit..−1, if upto < 0).
/// More precisely, an unsigned random value x is generated as in random; then z := x * limit / 2^256 is computed.
/// Ensure you've called [random.initialize] in advance to make it unpredictable!
fun random.range(limit: int): int
    asm "RAND"

/// Returns the current random seed used to generate pseudo-random numbers.
@pure
fun random.getSeed(): uint256
    asm "RANDSEED"

/// Sets the random seed to the provided value.
fun random.setSeed(seed: uint256): void
    asm "SETRAND"

/// Initializes (mixes) random seed with the provided value.
fun random.initializeBy(mixSeedWith: uint256): void
    asm "ADDRAND"

/// Initializes random seed with current time to make random generation unpredictable.
/// Typically, you call this function once before calling [random.uint256] / [random.range].
fun random.initialize(): void
    asm "LTIME" "ADDRAND"


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
    asm "CDATASIZEQ NULLSWAPIFNOT2 NULLSWAPIFNOT"

/// Similar to [cell.calculateSize], but accepting a `slice` [s] instead of a `cell`.
/// The returned value of `x` does not take into account the cell that contains the `slice` [s] itself;
/// however, the data bits and the cell references of [s] are accounted for in `y` and `z`.
@pure
fun slice.calculateSize(self, maxCells: int): (int, int, int, bool)
    asm "SDATASIZEQ NULLSWAPIFNOT2 NULLSWAPIFNOT"

/// A non-quiet version of [cell.calculateSize] that throws a cell overflow exception (`8`) on failure.
fun cell.calculateSizeStrict(self, maxCells: int): (int, int, int)
    asm "CDATASIZE"

/// A non-quiet version of [cell.calculateSize] that throws a cell overflow exception (`8`) on failure.
fun slice.calculateSizeStrict(self, maxCells: int): (int, int, int)
    asm "SDATASIZE"

/// Returns the depth of a `cell`.
/// If [c] has no references, then return `0`;
/// otherwise the returned value is one plus the maximum of depths of cells referred to from [c].
/// If [c] is a `null` instead of a cell, returns zero.
@pure
fun cell?.depth(self): int
    asm "CDEPTH"

/// Returns the depth of a `slice`.
/// If [s] has no references, then returns `0`;
/// otherwise the returned value is one plus the maximum of depths of cells referred to from [s].
@pure
fun slice.depth(self): int
    asm "SDEPTH"

/// Returns the depth of a `builder`.
/// If no cell references are stored in [b], then returns 0;
/// otherwise the returned value is one plus the maximum of depths of cells referred to from [b].
@pure
fun builder.depth(self): int
    asm "BDEPTH"

/// Returns the number of stack slots anyVariable occupies (works at compile-time).
/// Example: sizeof(nullableInt) = 1, because `int?` is 1 TVM slot holding either NULL or a value.
/// Example: sizeof(somePoint) = 2 for `struct Point { x:int, y: int }`: two fields one slot per each.
/// Useful for debugging or when preparing stack contents for RUNVM.
@pure
fun sizeof<T>(anyVariable: T): int
    builtin


/**
    Debug primitives.
    Only works for local TVM execution with debug level verbosity.
 */

/// `debug` is a built-in struct, it has only static methods.
/// Example: `debug.print(v)` and other methods.
struct debug

/// Dump a variable [x] to the debug log.
fun debug.print<T>(x: T): void
    builtin

/// Dump a string [x] to the debug log.
fun debug.printString<T>(x: T): void
    builtin

/// Dumps the stack (at most the top 255 values) and shows the total stack depth.
fun debug.dumpStack(): void
    builtin


/**
    Slice primitives: parsing cells.
    When you _load_ some data, you mutate the slice (shifting an internal pointer on the stack).
    When you _preload_ some data, you just get the result without mutating the slice.
 */

/// Compile-time function that converts a constant hex-encoded string to N/2 bytes.
/// Example: `const v = stringHexToSlice("abcdef")` = slice with 3 bytes `[ 0xAB, 0xCD, 0xEF ]`
/// Note: stringHexToSlice(slice_var) does not work! It accepts a constant string and works at compile-time.
@pure
fun stringHexToSlice(constStringBytesHex: slice): slice
    builtin

/// Converts a `cell` into a `slice`. Notice that [c] must be either an ordinary cell,
/// or an exotic cell (see [TVM.pdf](https://ton-blockchain.github.io/docs/tvm.pdf), 3.1.2)
/// which is automatically loaded to yield an ordinary cell `c'`, converted into a `slice` afterwards.
@pure
fun cell.beginParse(self): slice
    asm "CTOS"

/// Checks if slice is empty. If not, throws an exception with code 9.
fun slice.assertEnd(self): void
    asm "ENDS"

/// Loads the next reference from the slice.
@pure
fun slice.loadRef(mutate self): cell
    asm( -> 1 0) "LDREF"

/// Preloads the next reference from the slice.
@pure
fun slice.preloadRef(self): cell
    asm "PLDREF"

/// Loads a signed [len]-bit integer from a slice.
@pure
fun slice.loadInt(mutate self, len: int): int
    builtin

/// Loads an unsigned [len]-bit integer from a slice.
@pure
fun slice.loadUint(mutate self, len: int): int
    builtin

/// Loads the first `0 ≤ len ≤ 1023` bits from slice [s] into a separate slice `s''`.
@pure
fun slice.loadBits(mutate self, len: int): slice
    builtin

/// Preloads a signed [len]-bit integer from a slice.
@pure
fun slice.preloadInt(self, len: int): int
    builtin

/// Preloads an unsigned [len]-bit integer from a slice.
@pure
fun slice.preloadUint(self, len: int): int
    builtin

/// Preloads the first `0 ≤ len ≤ 1023` bits from slice [s] into a separate slice.
@pure
fun slice.preloadBits(self, len: int): slice
    builtin

/// Loads serialized amount of Toncoins (any unsigned integer up to `2^120 - 1`).
@pure
fun slice.loadCoins(mutate self): coins
    asm( -> 1 0) "LDGRAMS"

/// Loads bool (-1 or 0) from a slice
@pure
fun slice.loadBool(mutate self): bool
    asm( -> 1 0) "1 LDI"

/// Shifts a slice pointer to [len] bits forward, mutating the slice.
@pure
fun slice.skipBits(mutate self, len: int): self
    builtin

/// Returns the first `0 ≤ len ≤ 1023` bits of a slice.
@pure
fun slice.getFirstBits(self, len: int): slice
    asm "SDCUTFIRST"

/// Returns all but the last `0 ≤ len ≤ 1023` bits of a slice.
@pure
fun slice.removeLastBits(mutate self, len: int): self
    asm "SDSKIPLAST"

/// Returns the last `0 ≤ len ≤ 1023` bits of a slice.
@pure
fun slice.getLastBits(self, len: int): slice
    asm "SDCUTLAST"

/// Returns `0 ≤ len ≤ 1023` bits of a slice starting from `0 ≤ offset ≤ 1023`.
/// (in other words, extracts a bit substring `[offset, len)` out of the slice)
@pure
fun slice.getMiddleBits(self, offset: int, len: int): slice
    asm "SDSUBSTR"

/// Loads a dictionary (TL HashMapE structure, represented as TVM cell) from a slice.
/// Returns `null` if `nothing` constructor is used.
@pure
fun slice.loadDict(mutate self): dict
    asm( -> 1 0) "LDDICT"

/// Preloads a dictionary (cell) from a slice.
@pure
fun slice.preloadDict(self): dict
    asm "PLDDICT"

/// Loads a dictionary as [slice.loadDict], but returns only the remainder of the slice.
@pure
fun slice.skipDict(mutate self): self
    asm "SKIPDICT"

/// Loads (Maybe ^Cell) from a slice.
/// In other words, loads 1 bit: if it's true, loads the first ref, otherwise returns `null`.
@pure
fun slice.loadMaybeRef(mutate self): cell?
    asm( -> 1 0) "LDOPTREF"

/// Preloads (Maybe ^Cell) from a slice.
@pure
fun slice.preloadMaybeRef(self): cell?
    asm "PLDOPTREF"

/// Loads (Maybe ^Cell), but returns only the remainder of the slice.
@pure
fun slice.skipMaybeRef(mutate self): self
    asm "SKIPOPTREF"

/**
    Builder primitives: constructing cells.
    When you _store_ some data, you mutate the builder (shifting an internal pointer on the stack).
    All the primitives below first check whether there is enough space in the `builder`,
    and only then check the range of the value being serialized.
 */

/// Creates a new empty builder.
@pure
fun beginCell(): builder
    asm "NEWC"

/// Converts a builder into an ordinary `cell`.
@pure
fun builder.endCell(self): cell
    asm "ENDC"

/// Stores a reference to a cell into a builder.
@pure
fun builder.storeRef(mutate self, c: cell): self
    asm(c self) "STREF"

/// Stores a signed [len]-bit integer into a builder (`0 ≤ len ≤ 257`).
@pure
fun builder.storeInt(mutate self, x: int, len: int): self
    builtin

/// Stores an unsigned [len]-bit integer into a builder (`0 ≤ len ≤ 256`).
@pure
fun builder.storeUint(mutate self, x: int, len: int): self
    builtin

/// Stores a slice into a builder.
@pure
fun builder.storeSlice(mutate self, s: slice): self
    asm(s self) "STSLICE"

/// Stores an address into a builder.
@pure
fun builder.storeAddress(mutate self, addr: address): self
    asm(addr self) "STSLICE"

/// Stores amount of Toncoins into a builder.
@pure
fun builder.storeCoins(mutate self, x: coins): self
    builtin

/// Stores bool (-1 or 0) into a builder.
/// Attention: true value is `-1`, not 1! If you pass `1` here, TVM will throw an exception.
@pure
fun builder.storeBool(mutate self, x: bool): self
    builtin

/// Stores dictionary (represented by TVM `cell` or `null`) into a builder.
/// In other words, stores a `1`-bit and a reference to [c] if [c] is not `null` and `0`-bit otherwise.
@pure
fun builder.storeDict(mutate self, c: dict): self
    asm(c self) "STDICT"

/// Stores (Maybe ^Cell) into a builder.
/// In other words, if cell is `null`, store '0' bit; otherwise, store '1' and a ref to [c].
@pure
fun builder.storeMaybeRef(mutate self, c: cell?): self
    asm(c self) "STOPTREF"

/// Concatenates two builders.
@pure
fun builder.storeBuilder(mutate self, from: builder): self
    asm(from self) "STB"

/// Stores a slice representing TL addr_none$00 (two `0` bits).
@pure
fun builder.storeAddressNone(mutate self): self
    asm "b{00} STSLICECONST"


/**
    Slice size primitives.
 */

/// Returns the number of references in a slice.
@pure
fun slice.remainingRefsCount(self): int
    asm "SREFS"

/// Returns the number of data bits in a slice.
@pure
fun slice.remainingBitsCount(self): int
    asm "SBITS"

/// Returns both the number of data bits and the number of references in a slice.
@pure
fun slice.remainingBitsAndRefsCount(self): (int, int)
    asm "SBITREFS"

/// Checks whether a slice is empty (i.e., contains no bits of data and no cell references).
@pure
fun slice.isEmpty(self): bool
    asm "SEMPTY"

/// Checks whether a slice has no bits of data.
@pure
fun slice.isEndOfBits(self): bool
    asm "SDEMPTY"

/// Checks whether a slice has no references.
@pure
fun slice.isEndOfRefs(self): bool
    asm "SREMPTY"

/// Checks whether data parts of two slices coinside.
@pure
fun slice.bitsEqual(self, b: slice): bool
    asm "SDEQ"

/// Returns the number of cell references already stored in a builder.
@pure
fun builder.refsCount(self): int
    asm "BREFS"

/// Returns the number of data bits already stored in a builder.
@pure
fun builder.bitsCount(self): int
    asm "BBITS"


/**
    Address manipulation primitives.
 */

/// Compile-time function that parses a valid contract address.
/// Example: address("EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5")
/// Example: address("0:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8")
/// Returns `address`, which can be stored in a builder, compared with `==`, etc.
@pure
fun address(stdAddress: slice): address
    builtin

/// Creates a slice representing "none address" (TL addr_none$00 — two zero bits).
@pure
fun createAddressNone(): address
    asm "b{00} PUSHSLICE"

/// Returns if it's an empty address.
/// Don't confuse it with null! Empty address is a slice with two `0` bits.
/// In TL/B, it's addr_none$00.
@pure
fun address.isNone(self): bool
    asm "b{00} SDBEGINSQ" "NIP"

/// Returns if it's a standard (internal) address. Such addresses contain workchain (8 bits) and hash (256 bits).
/// All contract addresses are internal, so it's the most practical use case.
/// In TL/B it's addr_std$10.
/// For internal addresses, you can call [address.getWorkchain] and [address.getWorkchainAndHash].
@pure
fun address.isInternal(self): bool
    asm "b{10} SDBEGINSQ" "NIP"

/// Returns if it's an external address, used to communication with the outside world.
/// In TL/B it's addr_extern$01.
@pure
fun address.isExternal(self): bool
    asm "b{01} SDBEGINSQ" "NIP"

/// Extracts workchain and hash from a standard (internal) address.
/// If the address is not internal, throws a cell deserialization exception.
@pure
fun address.getWorkchainAndHash(self): (int8, uint256)
    asm "REWRITESTDADDR"

/// Extracts workchain from a standard (internal) address.
/// If the address is not internal, throws a cell deserialization exception.
@pure
fun address.getWorkchain(self): int8
    asm "REWRITESTDADDR" "DROP"

/// Checks whether two addresses are equal. Equivalent to `a == b`.
/// Deprecated! Left for smoother transition from FunC, where you used `slice` everywhere.
/// Use just `a == b` and `a != b` to compare addresses, don't use bitsEqual.
@pure
@deprecated("use `senderAddress == ownerAddress`, not `senderAddress.bitsEqual(ownerAddress)`")
fun address.bitsEqual(self, b: address): bool
    asm "SDEQ"

/// Loads from slice [s] a valid `MsgAddress` (none/internal/external).
@pure
fun slice.loadAddress(mutate self): address
    asm( -> 1 0) "LDMSGADDR"


/**
    Reserving Toncoins on balance and its flags.
 */

/// mode = 0: Reserve exact amount of nanotoncoins
const RESERVE_MODE_EXACT_AMOUNT = 0
/// +1: Actually reserves all but amount, meaning `currentContractBalance - amount`
const RESERVE_MODE_ALL_BUT_AMOUNT = 1
/// +2: Actually set `min(amount, currentContractBalance)` (without this mode, if amount is greater, the action will fail)
const RESERVE_MODE_AT_MOST = 2
/// +4: [amount] is increased by the _original_ balance of the current account (before the compute phase).
const RESERVE_MODE_INCREASE_BY_ORIGINAL_BALANCE = 4
/// +8: Actually sets `amount = -amount` before performing any further actions.
const RESERVE_MODE_NEGATE_AMOUNT = 8
/// +16: If this action fails, the transaction will be bounced.
const RESERVE_MODE_BOUNCE_ON_ACTION_FAIL = 16

/// Creates an output action which would reserve Toncoins on balance.
/// For [reserveMode] consider constants above.
fun reserveToncoinsOnBalance(nanoTonCoins: coins, reserveMode: int): void
    asm "RAWRESERVE"

/// Similar to [reserveToncoinsOnBalance], but also accepts a dictionary extraAmount (represented by a cell or null)
/// with extra currencies. In this way currencies other than Toncoin can be reserved.
fun reserveExtraCurrenciesOnBalance(nanoTonCoins: coins, extraAmount: dict, reserveMode: int): void
    asm "RAWRESERVEX"


/**
    Creating and sending messages.
    Basic scenario:
    ```
    val outMsg = createMessage({ ... options });   // you get OutMessage
    outMsg.send(mode);
    ```
 */

/// mode = 0 is used for ordinary messages; the gas fees are deducted from the senging amount; action phaes should NOT be ignored.
const SEND_MODE_REGULAR = 0
/// +1 means that the sender wants to pay transfer fees separately.
const SEND_MODE_PAY_FEES_SEPARATELY = 1
/// +2 means that any errors arising while processing this message during the action phase should be ignored.
const SEND_MODE_IGNORE_ERRORS = 2
/// in the case of action fail - bounce transaction. No effect if SEND_MODE_IGNORE_ERRORS (+2) is used. TVM UPGRADE 2023-07. https://docs.ton.org/learn/tvm-instructions/tvm-upgrade-2023-07#sending-messages
const SEND_MODE_BOUNCE_ON_ACTION_FAIL = 16
/// mode = 32 means that the current account must be destroyed if its resulting balance is zero.
const SEND_MODE_DESTROY = 32
/// mode = 64 is used for messages that carry all the remaining value of the inbound message in addition to the value initially indicated in the new message.
const SEND_MODE_CARRY_ALL_REMAINING_MESSAGE_VALUE = 64
/// mode = 128 is used for messages that are to carry all the remaining balance of the current smart contract (instead of the value originally indicated in the message).
const SEND_MODE_CARRY_ALL_BALANCE = 128
/// do not create an action, only estimate fee. TVM UPGRADE 2023-07. https://docs.ton.org/learn/tvm-instructions/tvm-upgrade-2023-07#sending-messages
const SEND_MODE_ESTIMATE_FEE_ONLY = 1024
/// Other modes affect the fee calculation as follows:
/// +64 substitutes the entire balance of the incoming message as an outcoming value (slightly inaccurate, gas expenses that cannot be estimated before the computation is completed are not taken into account).
/// +128 substitutes the value of the entire balance of the contract before the start of the computation phase (slightly inaccurate, since gas expenses that cannot be estimated before the completion of the computation phase are not taken into account).

type ExtraCurrenciesDict = dict

/// ContractState is "code + data" of a contract.
/// Used in outgoing messages (StateInit) to initialize a destination contract.
struct ContractState {
    code: cell
    data: cell
}

/// AddressShardingOptions provides settings to calculate an address in another shard.
/// Consider [createMessage] and [address.buildSameAddressInAnotherShard] for usage.
struct AddressShardingOptions {
    fixedPrefixLength: uint5        // shard depth, formerly splitDepth
    closeTo: address
}

/// AutoDeployAddress is a destination that initializes a receiver contract if it does not exist yet.
/// In order to do this, it contains StateInit — and calculates an address by a hash of StateInit.
/// Example:
/// ```
/// createMessage({
///     dest: {
///         workchain: 0,
///         stateInit: {
///             code: jettonWalletCode,
///             data: jettonWalletEmptyStorage.toCell()
///         }
///     },
///     ...
/// ```
/// You just provide code+data, and the compiler automatically calculates the destination address,
/// because in TON, the address of a contract, by definition, is a hash of its initial state.
/// You can also use it without sending a message. See [buildAddress] and [addressMatches].
struct AutoDeployAddress {
    workchain: int8 = BASECHAIN
    stateInit: ContractState | cell
    toShard: AddressShardingOptions? = null
}

/// Constructs an address that a deployed contract will have.
/// For example, from a jetton minter, you want to calculate an address of a jetton wallet:
/// ```
/// val jwDeployed = calcDeployedJettonWallet(...);
/// val jwAddrBuilt = jwDeployed.buildAddress();
/// ```
/// Just instead of `dest` for [createMessage], you use it is to calculate a jetton/nft address.
/// Note: returns `builder`, not `address`! It's cheap.
/// If you really need `address`, use `address.fromValidBuilder(res)`.
@pure
fun AutoDeployAddress.buildAddress(self): builder
    builtin

/// Checks that an address matches a deployed contract.
/// For example, from a jetton minter, you're checking whether a message is from a jetton wallet:
/// ```
/// val jwDeployed = calcDeployedJettonWallet(...);
/// val fromJW = jwDeployed.addressMatches(senderAddress);
/// ```
/// Just instead of `dest` for [createMessage], you use it is to check whether a sender is a jetton/nft.
@pure
fun AutoDeployAddress.addressMatches(self, addr: address): bool
    builtin

/// Options for creating an outgoing message.
/// Consider [createMessage] for examples.
struct CreateMessageOptions<TBody = never> {
    /// whether a message will bounce back on error
    bounce: bool
    /// message value: attached tons (or tons + extra currencies)
    value: coins | (coins, ExtraCurrenciesDict)
    /// destination is either a provided address, or is auto-calculated by stateInit
    dest: address               // either just send a message to some address
        | builder               // ... or a manually constructed builder with a valid address
        | (int8, uint256)       // ... or to workchain + hash (also known as accountID)
        | AutoDeployAddress     // ... or "send to stateInit" aka deploy (address auto-calculated)
    /// body is any serializable object (or just miss this field for empty body)
    body: TBody
}

/// Creates a message (`OutMessage`) — a well-formatted message cell.
/// Typically, you just send it. In advanced scenarios, you can estimate fees or even postpone sending.
/// Example:
/// ```
/// val reply = createMessage({
///     bounce: false,
///     value: ton("0.05"),
///     dest: senderAddress,
///     body: RequestedInfo { ... }        // note: no `toCell`! just pass an object
/// });
/// reply.send(SEND_MODE_REGULAR);
/// ```
/// Hint: don't call `body.toCell()`, pass `body` directly!
///       (if body is small, it will be inlined without an expensive cell creation)
///       (if body is large, the compiler will automatically wrap it into a cell)
/// If you need an empty body, just miss the field `body`, fill other 3 fields.
@pure
fun createMessage<TBody>(options: CreateMessageOptions<TBody>): OutMessage
    builtin


/// ExtOutLogBucket is a variant of a custom external address for emitting logs "to the outer world".
/// It includes some "topic" (arbitrary number), that determines the format of the message body.
/// For example, you emit "deposit event" (reserving topic "deposit" = 123):
/// > `dest: ExtOutLogBucket { topic: 123 }, body: DepositData { ... }`
/// and external indexers can index your emitted logs by destination address without parsing body.
/// Currently, external messages are used only for emitting logs (for viewing them in indexers).
/// In the future, there might be
/// > 0x01 ExtOutOffchainContract { adnl: uint256 | bits256 }
/// Serialization details: '01' (addr_extern) + 256 (len) + 0x00 (prefix) + 248 bits = 267 in total
struct (0x00) ExtOutLogBucket {
    topic: uint248 | bits248
}

/// Options for creating an external outgoing message.
/// Consider [createExternalLogMessage] for examples.
struct CreateExternalLogMessageOptions<TBody = never> {
    /// destination is either an external address or a pattern to calculate it
    dest: address           // either some valid external/none address (not internal!)
        | builder           // ... or a manually constructed builder with a valid external address
        | ExtOutLogBucket   // ... or encode topic/eventID in destination
    /// body is any serializable object (or just miss this field for empty body)
    body: TBody
}

/// Creates an external message (`OutMessage`) — a well-formatted message cell.
/// Typically, you just send it. In advanced scenarios, you can estimate fees or even postpone sending.
/// Example:
/// ```
/// val emitMsg = createExternalLogMessage({
///     dest: createAddressNone(),
///     body: DepositEvent { ... }        // note: no `toCell`! just pass an object
/// });
/// emitMsg.send(SEND_MODE_REGULAR);
/// ```
/// Note: `createMessage` also returns `OutMessage`, it's okay: a composed message cell is universal.
/// Hint: don't call `body.toCell()`, pass `body` directly!
///       (if body is small, it will be inlined without an expensive cell creation)
///       (if body is large, the compiler will automatically wrap it into a cell)
@pure
fun createExternalLogMessage<TBody>(options: CreateExternalLogMessageOptions<TBody>): OutMessage
    builtin

/// UnsafeBodyNoRef is used to prevent default behavior: when message body is potentially large,
/// it's packed into a separate ref. Wrapping body with this struct tells the compiler:
/// "inline message body in-place, I guarantee it will fit".
/// Example:
/// ```
/// struct ProbablyLarge { a: (coins, coins, coins, coins, coins) }  // max 620 bits
///
/// val contents: ProbablyLarge = { ... };  // you are sure: values are small
/// createMessage({
///     // body: contents,                  // by default, it will be stored a ref (it's large)
///     body: UnsafeBodyNoRef {
///         forceInline: contents,          // but wrapping forces the compiler to inline it
///     }
/// ```
/// Another example: your body contains `builder` or `RemainingBitsAndRefs`: unpredictable size.
/// If you guarantee it's small and refs won't clash with code/data, avoid creating a cell.
struct UnsafeBodyNoRef<T> {
    forceInline: T
}

/// OutMessage is a result of [createMessage].
/// Essentially, it's a composed message cell, ready to be sent.
struct OutMessage {
    messageCell: cell
}

/// Sends a ready message cell.
/// For `sendMode`, see the constants above (SEND_MODE_*).
fun OutMessage.send(self, sendMode: int): void
    asm "SENDRAWMSG"

fun OutMessage.sendAndEstimateFee(self, sendMode: int): coins
    asm "SENDMSG"

@pure
fun OutMessage.estimateFeeWithoutSending(self, sendMode: int): coins
    asm "10 PUSHPOW2" "OR" "SENDMSG"

@pure
fun OutMessage.hash(self): uint256
    asm "HASHCU"


/// StateInit is a "canonical TL/B representation from block.tlb" of a contract initial state.
/// But for everyday tasks, it's too complicated. It is not used in practice.
/// To represent code+data, consider [ContractState].
/// To represent sharding (fixedPrefixLength, formerly splitDepth), consider [AutoDeployAddress].
struct StateInit {
    fixedPrefixLength: uint5?
    special: (bool, bool)?
    code: cell?
    data: cell?
    library: cell?
}

/// Calculates a hash of StateInit if only code+data are set.
/// Example:
/// ```
/// val addrHash = StateInit.calcHashCodeData(codeCell, dataCell);
/// createMessage({
///     dest: (workchain, addrHash),
///     ...
/// ```
@pure
fun StateInit.calcHashCodeData(code: cell, data: cell): uint256 asm
"""                 // code data
    DUP2            // code data code data
    HASHCU
    SWAP
    HASHCU          // code data dataHash codeHash
    SWAP2           // dataHash codeHash code data
    CDEPTH
    SWAP
    CDEPTH          // dataHash codeHash dataDepth codeDepth

    NEWC
    x{020134} STSLICECONST // store refs_descriptor | bits_descriptor | data
    16 STU                 // store codeDepth
    16 STU                 // store dataDepth
    256 STU                // store codeHash
    256 STU                // store dataHash

    ONE HASHEXT_SHA256
"""

/// Calculates a hash of StateInit if fixedPrefixLength+code+data are set.
@pure
fun StateInit.calcHashPrefixCodeData(fixedPrefixLength: uint5, code: cell, data: cell): uint256 asm
"""                 // pDepth code data
    DUP2            // pDepth code data code data
    HASHCU
    SWAP
    HASHCU          // pDepth code data dataHash codeHash
    SWAP2           // pDepth dataHash codeHash code data
    CDEPTH
    SWAP
    CDEPTH          // pDepth dataHash codeHash dataDepth codeDepth

    s4 PUSH         // pDepth dataHash codeHash dataDepth codeDepth pDepth
    10 LSHIFT#
    0x020381A0 PUSHINT
    ADD             // calc refs_descriptor | bits_descriptor | data

    NEWC
    32 STU
    16 STU
    16 STU
    256 STU
    256 STU

    ONE HASHEXT_SHA256     // pDepth hash
    NIP
"""


/// Given an internal address A="aaaa...a" returns "bbaa...a" (D bits from address B, 256-D from A).
/// Example for fixedPrefixLength (shard depth) = 8:
/// | self (A)     | aaaaaaaaaaa...aaa |
/// | closeTo (B)  | 01010101bbb...bbb |   shardPrefix = 01010101 (depth 8)
/// | result       | 01010101aaa...aaa |   address of A in same shard as B
/// More precisely, self (input) is 267 bits: '100' (std addr no anycast) + workchainA + "aaaa...a".
/// The result is also 267 bits: '100' + workchainB + "bb" (D bits) + "aa...a" (256-D bits).
/// Note: returns `builder`, not `address`! Because builder is cheap, and you can send a message to it:
/// ```
/// createMessage({
///     dest: resBuilder, // a builder containing a valid address can be passed to createMessage()
///     ...
/// ```
/// If you really need `address`, use `address.fromValidBuilder(resBuilder)`.
@pure
fun address.buildSameAddressInAnotherShard(self, options: AddressShardingOptions): builder
    builtin

/// Converts a builder containing a valid address (internal/external/none) to `address` (slice under the hood).
/// Gas-expensive! Because or a cell creation: essentially, it's `b.endCell().beginParse()`.
/// Example: you've manually built a 267-bit address: '100' (std addr no anycast) + workchain + hash
///          and you want to return `address` (TVM slice) from a contract getter:
/// ```
/// get calcWalletAddress(ownerAddress: address): address {
///     val b = calculate(...);             // manually calculate into builder
///     return address.fromValidBuilder(b)  // but really need address
/// }
/// ```
/// Hint: if you want just to send a message, don't call this function, use a builder directly:
/// ```
/// createMessage({
///     dest: someBuilder,   // a builder containing a valid address can be passed to createMessage()
///     ...                  // so, don't call this expensive function in this case
/// ```
@pure
fun address.fromValidBuilder(b: builder): address
    asm "ENDC" "CTOS"


/// Sends a raw message — a correctly serialized TL object `Message X`.
/// In practice, you'll use a high-level wrapper [createMessage].
fun sendRawMessage(msg: cell, mode: int): void
    asm "SENDRAWMSG"



/**
    Receiving and handling messages.
*/

/// InMessage is an input for `onInternalMessage` — when your contract accepts a non-bounced message.
/// Internally, some data exist on the stack, some can be acquired with TVM instructions.
/// The compiler replaces `in.someField` and gets a value correctly.
/// Example:
/// ```
/// fun onInternalMessage(in: InMessage) {
///     in.senderAddress   // actually calls `INMSG_SRC` asm instruction
///     in.body            // actually gets from a TVM stack
/// ```
struct InMessage {
    senderAddress: address          // an internal address from which the message arrived
    valueCoins: coins               // ton amount attached to an incoming message
    valueExtra: dict                // extra currencies attached to an incoming message
    originalForwardFee: coins       // fee that was paid by the sender
    createdLt: uint64               // logical time when a message was created
    createdAt: uint32               // unixtime when a message was created
    body: slice                     // message body, parse it with `lazy AllowedMsg.fromSlice(in.body)`
}

/// InMessageBounced is an input for `onBouncedMessage`.
/// Very similar to a non-bounced input [InMessage].
/// Note, that `bouncedBody` is currently 256 bits: 0xFFFFFFFF + 224 bits from the originally sent message.
/// Parse it with care!
/// Example:
/// ```
/// fun onBouncedMessage(in: InMessageBounced) {
///     in.bouncedBody.skipBouncedPrefix();
///     val originalOpcode = in.bouncedBody.loadUint(32);
/// ```
struct InMessageBounced {
    senderAddress: address          // an internal address from which the message was bounced
    valueCoins: coins               // ton amount attached to a message
    valueExtra: dict                // extra currencies attached to a message
    originalForwardFee: coins       // comission that the sender has payed to send this message
    createdLt: uint64               // logical time when a message was created (and bounced)
    createdAt: uint32               // unixtime when a message was created (and bounced)
    bouncedBody: slice              // currently 256 bits: 0xFFFFFFFF + 224 bits sent originally
}

/// Skip 0xFFFFFFFF prefix (when a message is bounced).
@pure
fun slice.skipBouncedPrefix(mutate self): self
    asm "32 LDU" "NIP"

/// Load msgFlags from incoming message body (4 bits).
@pure
@deprecated("use `onBouncedMessage` handler instead of parsing msgCell flags manually")
fun slice.loadMessageFlags(mutate self): int
    asm( -> 1 0) "4 LDU"

/// Loads a message opcode (32-bit integer) when parsing message body manually.
@pure
fun slice.loadMessageOp(mutate self): int
    asm( -> 1 0) "32 LDU"

/// Stores a message opcode (32-bit integer) when composing an output message manually.
@pure
@deprecated("use `createMessage` instead of composing messages manually")
fun builder.storeMessageOp(mutate self, op: int): self
    asm(op self) "32 STU"

/// Loads a message queryId (64-bit integer, immediately following the opcode) when parsing message body manually.
@pure
@deprecated("use structures and lazy match instead of parsing messages manually")
fun slice.loadMessageQueryId(mutate self): int
    asm( -> 1 0) "64 LDU"

@pure
@deprecated("use structures and lazy loading instead of parsing messages manually")
fun slice.skipMessageQueryId(mutate self): self
    asm "64 LDU" "NIP"

@pure
@deprecated("use `createMessage` instead of composing messages manually")
fun builder.storeMessageQueryId(mutate self, queryId: int): self
    asm(queryId self) "64 STU"


