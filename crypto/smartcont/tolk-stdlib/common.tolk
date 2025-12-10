// Standard library for Tolk (LGPL licence).
// It contains common functions that are available out of the box, the user doesn't have to import anything.
// More specific functions are required to be imported explicitly, like "@stdlib/gas-payments".
tolk 1.2

/**
    Built-in types.
 */

/// `int` is the primitive 257-bit signed integer type.
type int = builtin

/// `bool` is a classic boolean type, which can hold only two values: `true` and `false`.
/// At the TVM (TON virtual machine) level, it's an integer -1 or 0.
/// Note: `boolVar as int` is possible, but remember, that true is -1, not 1!
type bool = builtin

/// `cell` is a data structure, which can hold of up to 1023 bits (not bytes!)
/// and up to 4 references (refs) to other cells.
/// Both contract code and contract state are represented by a tree of cells.
/// [See docs](https://docs.ton.org/v3/documentation/data-formats/tlb/cell-boc)
type cell = builtin

/// `slice` is a "cell opened for reading".
/// When you call [cell.beginParse], you get a `slice`, from which you can load binary data
/// or high-level structures with [T.fromSlice].
type slice = builtin

/// `builder` is a "cell at the stage of creation".
/// When you call [beginCell], you get a `builder`, populate it with binary data or structures,
/// and after [builder.endCell], you get a `cell`.
type builder = builtin

/// `continuation` is an "executable cell" representing executable TVM bytecode.
/// They are used to manage execution flow in TVM programs and serve as
/// the basis for function calls, exception handling, and control flow operations. 
type continuation = builtin

/// `tuple` is a collection from 0 to 255 elements of any type.
/// You can push, pop, access individual elements as `someTuple.0`.
/// A tuple occupies one stack slot regardless of its size.
type tuple = builtin

/// `address` represents a standard (internal) address: workchain + hash.
/// Addresses are comparable with `==` operator, they have methods like [address.getWorkchain].
/// In binary, it's 267 bits: '10' (addr_std) + '0' (no anycast) + workchain (int8) + hash (uint256).
/// If you wish to describe "a potentially missing address", use `address?` (nullable).
/// When set to `null`, `address?` will be serialized as '00' ("none address"):
/// '00' (two zero bits) is a standard representation of a "missing" ("none") address.
/// At the TVM level, addresses are slices, so it's possible to  `someAddress as slice`.
/// [See docs](https://docs.ton.org/learn/overviews/addresses#address-of-smart-contract)
type address = builtin

/// `any_address` represents an internal/external/none address.
/// You can test it with [any_address.isExternal] and similar methods.
/// If you wish to describe "internal or none address", use `address?` (nullable)
/// instead of "any". That's why `any_address` is used in practice extremely rarely.
/// At the TVM level, addresses are slices, so it's possible to  `anyAddress as slice`.
/// [See docs](https://docs.ton.org/learn/overviews/addresses#address-of-smart-contract)
type any_address = builtin

/// `never` is a special type that represents computations that never complete normally.
/// A function that always throws an exception returns `never`, and the compiler knows
/// that any code after it is unreachable.
type never = builtin

/// `int8`, `int32`, `int222`, etc. is "a fixed-width signed integer with N bits", N <= 257.
/// Note: it's still `int` at runtime, you can assign "100500" to "int8":
/// overflow will happen at serialization to a cell/builder, NOT at assignment. 
type intN = builtin

/// `uint32`, `uint64`, `uint111`, etc. is "a fixed-width unsigned integer with N bits", N <= 256.
/// Note: it's still `int` at runtime, you can assign "100500" to "uint8":
/// overflow will happen at serialization to a cell/builder, NOT at assignment. 
type uintN = builtin

/// `coins` is a special primitive representing "nanotoncoins". One TON = 10^9 nanotoncoins.
/// You can create coins with `ton()` function: `ton("0.05")` (actually, `int` 50000000 at runtime).
/// Arithmetic operations on `coins` degrade to 257-bit `int` type.
type coins = builtin

/// `varint16` is `int` at runtime, but serialized as "variadic signed int", -2^119 <= X < 2^119. 
type varint16 = builtin

/// `varuint16` is `int` at runtime, but serialized as "variadic unsigned int", 0 <= X < 2^120. 
type varuint16 = builtin

/// `varint32` is `int` at runtime, but serialized as "variadic signed int", -2^247 <= X < 2^247. 
type varint32 = builtin

/// `varuint32` is `int` at runtime, but serialized as "variadic unsigned int", 0 <= X < 2^248. 
type varuint32 = builtin

/// `bits256`, `bits111`, etc. is "a fixed-width slice with N bits and 0 refs", N <= 1023.
/// Note: use `as` operator to convert `slice` to `bitsN`: `someSlice as bits256` 
/// (manually writing `as` enforces you to think that this conversion is correct).
/// Note: similar to `intN`, you can assign an invalid slice to `bitsN`,
/// an error will be fired at serialization with [T.toCell] and similar, NOT at assignment.
type bitsN = builtin

/// `bytes8`, `bytes99`, etc. is a convenient alias for `bits(N*8)`
type bytesN = builtin

/// `map<K, V>` is "a map from a key K to a value V".
/// Internally, it's an "optional cell": an empty map is `null`, a non-empty points to a root cell.
/// Restrictions for K and V types:
/// - a key must be fixed-width; valid: `int32`, `uint64`, `address`, `bits256`, `Point`; invalid: `int`, `coins`
/// - a value must be serializable; valid: `int32`, `coins`, `AnyStruct`, `Cell<AnyStruct>`; invalid: `int`, `builder`
struct map<K, V> {
    private tvmDict: dict
}

/// `dict` is a low-level TVM dictionary.
/// Think of it as "a map with unknown keys and unknown values".
/// Prefer using `map<K, V>`, not `dict`.
type dict = cell?

/// `void` is the unit type representing the absence of a meaningful value.
/// It's similar to both `void` and `unit` in other languages.
/// Note: a function without return type means "auto infer", NOT "void".
type void = builtin

/// `Cell<T>` represents a typed cell reference (as opposed to untyped `cell`).
/// ```
/// struct ExtraData { ... }
///
/// struct MyStorage {
///     ...
///     extra:    Cell<ExtraData>    // TL-B `^ExtraData`
///     optional: Cell<ExtraData>?   // TL-B `(Maybe ^ExtraData)`
///     code:     cell               // TL-B `^Cell`
///     data:     cell?              // TL-B `(Maybe ^Cell)`
/// }
/// ```
/// Note, that `st = MyStorage.fromSlice(s)` does NOT deep-load any refs; `st.extra` is `Cell<T>`, not `T`;
/// you should manually call `st.extra.load()` to get `T` (ExtraData in this example).
struct Cell<T> {
    private readonly tvmCell: cell
}

/// Get human-readable type name of any T for logging or low-level purposes.
/// Example:
/// ```
/// debug.printString(int.typeName());  // "int"
/// ```
@pure
fun T.typeName(): slice
    builtin

/// Get human-readable type name of any object for logging or low-level purposes.
/// Example:
/// ```
/// debug.printString(42.typeNameOfObject());   // "int"
/// ```
@pure
fun T.typeNameOfObject(self): slice
    builtin

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

/// Returns the `i`-th element of a tuple.
/// `t.i` is actually the same as `t.get(i)`
@pure
fun tuple.get<T>(self, index: int): T
    builtin

/// Sets the `i`-th element of a tuple to a specified value
/// (an element with this index must already exist, a new element is not created).
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

/// Packs any object from a stack to a tuple. 
/// An object occupies N slots on a stack — the tuple will be of size N.
/// Example:
/// ```
/// struct Point { x: int, y: int }
/// 
/// var p: Point = { x: 1, y: 2 };
/// var t = p.toTuple();     // [ 1 2 ]
/// p = Point.fromTuple(t);  // back
/// t.get<int>(0);           // 1
/// ```
/// It can be used for logging or low-level purposes like running a child VM or custom "equals".
/// See [T.fromTuple] counterpart.
@pure
fun T.toTuple(self): tuple
    builtin

/// Unpacks a tuple back to an object on a stack.
/// See [T.toTuple] for explanation and examples.
@pure
fun T.fromTuple(packedObject: tuple): T
    builtin

/**
    Mathematical primitives.
 */

/// Converts a constant floating-point string to nanotoncoins.
/// Example: `ton("0.05")` is equal to 50000000.
/// Note, that `ton()` requires a constant string; `ton(some_var)` is an error.
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

/// Computes the quotient and remainder of x / y. Example: divMod(112,3) = (37,1)
@pure
fun divMod(x: int, y: int): (int, int)
    asm "DIVMOD"

/// Computes the remainder and quotient of x / y. Example: modDiv(112,3) = (1,37)
@pure
fun modDiv(x: int, y: int): (int, int)
    asm(-> 1 0) "DIVMOD"

/// Computes multiple-then-divide: floor(x * y / z).
/// The intermediate result is stored in a 513-bit integer to prevent precision loss.
@pure
fun mulDivFloor(x: int, y: int, z: int): int
    builtin

/// Similar to [mulDivFloor], but rounds the result: round(x * y / z).
@pure
fun mulDivRound(x: int, y: int, z: int): int
    builtin

/// Similar to [mulDivFloor], but ceils the result: ceil(x * y / z).
@pure
fun mulDivCeil(x: int, y: int, z: int): int
    builtin

/// Computes the quotient and remainder of (x * y / z). Example: mulDivMod(112,3,10) = (33,6)
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
@pure
fun contract.getOriginalBalance(): coins
    asm "BALANCE" "FIRST"

/// Same as [contract.getOriginalBalance], but returns a tuple: balance in nanotoncoins and extra currencies.
@pure
fun contract.getOriginalBalanceWithExtraCurrencies(): [coins, ExtraCurrenciesMap]
    asm "BALANCE"

/// Returns the persistent contract storage cell. It can be parsed or modified with slice and builder primitives later.
@pure
fun contract.getData(): cell
    asm "c4 PUSH"

/// Sets the persistent contract storage. 
fun contract.setData(c: cell): void
    asm "c4 POP"

/// Retrieves code of smart-contract from c7.
@pure
fun contract.getCode(): cell
    asm "MYCODE"

/// Creates an output action that would change this smart contract code after successful termination of the current run.
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

/// Commits current state of registers `c4` (persistent data) and `c5` (actions)
/// so that the current execution is considered "successful" with the saved values even if an exception
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
    skipBitsNValidation: bool = false
}

/// UnpackOptions allows you to control behavior of `MyStruct.fromCell(c)` and similar functions.
struct UnpackOptions {
    /// after finished reading all fields from a cell/slice, call [slice.assertEnd] to ensure no remaining data left;
    /// it's the default behavior, it ensures that you've fully described data you're reading with a struct;
    /// example: `struct Point { x: int8; y: int8 }`, input "0102" is ok, "0102FF" will throw excno 9;
    /// note: setting this to false does not decrease gas (DROP from a stack and ENDS cost the same);
    /// note: this option controls [T.fromCell] and [T.fromSlice], but is ignored by [slice.loadAny];
    /// note: `lazy` ignores this option, because it reads fields on demand or even skips them
    assertEndAfterReading: bool = true

    /// this excNo is thrown if a prefix doesn't match, e.g. for `struct (0x01) A` given input "88...";
    /// similarly, for a union type, this is thrown when none of the opcodes match;
    /// note: `lazy` ignores this option if you have `else` in `match` (you write custom logic there)
    throwIfOpcodeDoesNotMatch: int = 63
}

/// Convert anything to a cell (most likely, you'll call it for structures).
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
/// ```
/// var st = MyStorage.fromCell(contract.getData());
/// ```
/// Internally, a cell is unpacked to a slice, and that slice is parsed
/// (packedCell.beginParse() + read from slice).
@pure
fun T.fromCell(packedCell: cell, options: UnpackOptions = {}): T
    builtin

/// Parse anything from a slice (most likely, you'll call it for structures).
/// ```
/// var msg = CounterIncrement.fromSlice(cs);
/// ```
/// All fields are read from a slice immediately unless `lazy`.
/// If a slice is corrupted, an exception is thrown (most likely, excode 9 "cell underflow").
/// Note, that a passed slice is NOT mutated, its internal pointer is NOT shifted.
/// If you need to mutate it, like `cs.loadInt()`, consider calling `cs.loadAny<CounterIncrement>()`.
@pure
fun T.fromSlice(rawSlice: slice, options: UnpackOptions = {}): T
    builtin

/// Parse anything from a slice, shifting its internal pointer.
/// Similar to `slice.loadUint()` and others, but allows loading structures.
/// ```
/// var st: MyStorage = cs.loadAny();     // or cs.loadAny<MyStorage>()
/// ```
/// Similar to `MyStorage.fromSlice(cs)`, but called as a slice method and mutates the slice.
/// Note: `options.assertEndAfterReading` is ignored by this function, because it's actually intended
/// to read data from the middle.
@pure
fun slice.loadAny<T>(mutate self, options: UnpackOptions = {}): T
    builtin

/// Skip anything in a slice, shifting its internal pointer.
/// Similar to `slice.skipBits()` and others, but allows skipping structures.
/// ```
/// struct TwoInts { a: int32; b: int32; }
/// cs.skipAny<TwoInts>();    // skips 64 bits
/// ```
@pure
fun slice.skipAny<T>(mutate self, options: UnpackOptions = {}): self
    builtin

/// Store anything to a builder.
/// Similar to `builder.storeUint()` and others, but allows storing structures.
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

/// Parse data from already loaded cell reference.
/// ```
/// struct MyStorage { ... extra: Cell<ExtraData> }
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

/// `RemainingBitsAndRefs` is a special built-in type to get "all the rest" slice tail on reading.
/// ```
/// struct JettonMessage {
///     ... some fields
///     forwardPayload: RemainingBitsAndRefs;
/// }
/// ```
/// When you deserialize JettonMessage, forwardPayload contains "everything left after reading fields above".
type RemainingBitsAndRefs = slice

/// Creates a cell with zero bits and references.
/// Equivalent to `beginCell().endCell()`.
@pure
fun createEmptyCell(): cell
    asm "<b b> PUSHREF"

/// Creates a slice with zero remaining bits and references.
/// Equivalent to `beginCell().endCell().beginParse()`.
@pure
fun createEmptySlice(): slice
    asm "x{} PUSHSLICE"


/**
    Signature checks, hashing, cryptography.
 */

/// Compile-time function that calculates crc32 of a constant string.
/// ```
/// const op = stringCrc32("some_str") // = 4013618352 = 0xEF3AF4B0
/// ```
/// Note: `stringCrc32(slice_var)` does NOT work! It accepts a constant string and works at compile-time.
@pure
fun stringCrc32(constString: slice): int
    builtin

/// Compile-time function that calculates crc16 (XMODEM) of a constant string.
/// ```
/// const op = stringCrc16("some_str") // = 53407 = 0xD09F
/// ```
/// Note: `stringCrc16(slice_var)` does NOT work! It accepts a constant string and works at compile-time.
@pure
fun stringCrc16(constString: slice): int
    builtin

/// Compile-time function that calculates sha256 of a constant string and returns 256-bit integer.
/// ```
/// const hash = stringSha256("some_crypto_key")
/// ```
/// Note: it's a compile-time function, `stringSha256(slice_var)` does NOT work!
/// Use [slice.bitsHash] or [slice.hash] (declared below) to hash a slice without/with its refs at runtime.
@pure
fun stringSha256(constString: slice): int
    builtin

/// Compile-time function that calculates sha256 of a constant string and takes the first 32 bits.
/// ```
/// const minihash = stringSha256_32("some_crypto_key")
/// ```
/// Note: `stringSha256_32(slice_var)` does NOT work! It accepts a constant string and works at compile-time.
@pure
fun stringSha256_32(constString: slice): int
    builtin

/// Compile-time function that takes N-chars ascii string and interprets it as a number in base 256.
/// ```
/// const value = stringToBase256("AB") // = 16706 (65*256 + 66)
/// ```
/// Note: `stringToBase256(slice_var)` does NOT work! It accepts a constant string and works at compile-time.
@pure
fun stringToBase256(constString: slice): int
    builtin

/// Computes the representation hash of a cell and returns it as a 256-bit unsigned integer.
/// Useful for signing and checking signatures of arbitrary entities represented by a tree of cells.
@pure
fun cell.hash(self): uint256
    asm "HASHCU"

/// Computes the hash of a slice and returns it as a 256-bit unsigned integer.
/// The result is the same as [cell.hash] for a cell containing data and references from this slice.
@pure
fun slice.hash(self): uint256
    asm "HASHSU"

/// Computes the hash of a builder and returns it as a 256-bit unsigned integer.
/// The same as "transform builder to cell + calc hash of that cell", but without cell creation.
@pure
fun builder.hash(self): uint256
    asm "HASHBU"

/// Computes sha256 of the data bits of a slice (without refs). 
/// If the bit length is not divisible by eight, throws a cell underflow exception.
@pure
fun slice.bitsHash(self): uint256
    asm "SHA256U"

/// Checks the Ed25519-`signature` of a `hash` (a 256-bit unsigned integer, usually computed as the hash of some data)
/// using `publicKey` (also represented by a 256-bit unsigned integer).
/// The signature must contain at least 512 data bits; only the first 512 bits are used.
/// Note that `CHKSIGNU` creates a 256-bit slice with the hash and calls `CHKSIGNS`.
/// That is, if parameter `hash` is computed as the hash of some data, these data are hashed twice,
/// the second hashing occurring inside `CHKSIGNS`.
@pure
fun isSignatureValid(hash: int, signature: slice, publicKey: int): bool
    asm "CHKSIGNU"

/// Checks whether `signature` is a valid Ed25519-signature of the data portion of slice `data` using `publicKey`,
/// similarly to [isSignatureValid].
/// If the bit length of `data` is not divisible by eight, throws a cell underflow exception.
/// The verification of Ed25519 signatures is the standard one,
/// with sha256 used to reduce `data` to the 256-bit number that is actually signed.
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
/// in a tree of cells, effectively returning the total storage used by this tree taking into account
/// the identification of equal cells.
/// The values of `x`, `y`, and `z` are computed by a depth-first traversal of this tree,
/// with a hash table of visited cell hashes used to prevent visits of already-visited cells.
/// The total count of visited cells `x` cannot exceed non-negative `maxCells`;
/// otherwise the computation is aborted before visiting the `(maxCells + 1)`-st cell and
/// a zero flag is returned to indicate failure.
@pure
fun cell.calculateSize(self, maxCells: int): (int, int, int, bool)
    asm "CDATASIZEQ NULLSWAPIFNOT2 NULLSWAPIFNOT"

/// Similar to [cell.calculateSize], but accepting a slice instead of a cell.
/// The returned value of `x` does not take into account the cell that contains the slice itself;
/// however, its data bits and cell references are accounted for in `y` and `z`.
@pure
fun slice.calculateSize(self, maxCells: int): (int, int, int, bool)
    asm "SDATASIZEQ NULLSWAPIFNOT2 NULLSWAPIFNOT"

/// A non-quiet version of [cell.calculateSize] that throws a cell overflow exception (`8`) on failure.
fun cell.calculateSizeStrict(self, maxCells: int): (int, int, int)
    asm "CDATASIZE"

/// A non-quiet version of [slice.calculateSize] that throws a cell overflow exception (`8`) on failure.
fun slice.calculateSizeStrict(self, maxCells: int): (int, int, int)
    asm "SDATASIZE"

/// Returns the depth of a cell: 0 if no references, otherwise 1 + maximum of depths of all references.
/// When called for `null`, returns 0.
@pure
fun cell?.depth(self): int
    asm "CDEPTH"

/// Returns the depth of a cell: 0 if no references, otherwise 1 + maximum of depths of all references.
@pure
fun slice.depth(self): int
    asm "SDEPTH"

/// Returns the depth of a builder: 0 if no references, otherwise 1 + maximum of depths of all references.
@pure
fun builder.depth(self): int
    asm "BDEPTH"

/// Returns the number of stack slots `anyVariable` occupies (works at compile-time).
/// - sizeof(nullableInt) = 1, because `int?` is 1 TVM slot holding either NULL or a value.
/// - sizeof(somePoint) = 2 for `struct Point { x:int, y:int }`: two fields one slot per each.
/// 
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

/// Dump a variable to the debug log.
fun debug.print<T>(x: T): void
    builtin

/// Dump a string to the debug log.
fun debug.printString<T>(x: T): void
    builtin

/// Dumps the stack (at most the top 255 values) and shows the total stack depth.
fun debug.dumpStack(): void
    builtin


/*
    Implementation of `map<K, V>` and methods over it.

    Note: maps in TVM are called "dictionaries". They are stored as nested cells 
  (each value in a separate cell, probably with intermediate cells-nodes).
  Constructing even small dicts is gas-expensive, because cell creation costs a lot.

    `map<K, V>` is a "human-readable interface" over low-level dictionaries. The compiler,
  knowing types of K and V, effectively generates asm instructions for storing/loading values.
  But still, of course, it's a tree of cells, it's just a TVM dictionary.
 */

/// Returns an empty typed map. It's essentially "PUSHNULL", since TVM NULL represents an empty map.
/// ```
/// var m: map<int8, int32> = createEmptyMap();
/// ```
@pure
fun createEmptyMap<K, V>(): map<K, V>
    builtin

/// Converts a low-level TVM dictionary to a typed map.
/// Actually, does nothing: accepts an "optional cell" and returns the same "optional cell",
/// so if you specify key/value types incorrectly, it will fail later, at `map.get` and similar.
@pure
fun createMapFromLowLevelDict<K, V>(d: dict): map<K, V>
    builtin

/// Converts a high-level map to a low-level TVM dictionary.
/// Actually, does nothing: returns the same "optional cell".
@pure
fun map<K, V>.toLowLevelDict(self): dict
    asm "NOP"

/// Checks whether a map is empty (whether a cell is null).
/// 
/// Note: a check `m == null` will not work, use `m.isEmpty()`.
@pure
fun map<K, V>.isEmpty(self): bool
    asm "DICTEMPTY"

/// Checks whether a key exists in a map.
@pure
fun map<K, V>.exists(self, key: K): bool
    builtin

/// Gets an element by key. If not found, does NOT throw, just returns isFound = false.
/// ```
/// val r = m.get(123);
/// if (r.isFound) {
///     r.loadValue()
/// }
/// ```
@pure
fun map<K, V>.get(self, key: K): MapLookupResult<V>
    builtin

/// Gets an element by key and throws if it doesn't exist.
@pure
fun map<K, V>.mustGet(self, key: K, throwIfNotFound: int = 9): V
    builtin

/// Sets an element by key.
/// ```
/// m.set(k, 3);
/// ```
/// Since it returns `self`, calls may be chained.
@pure
fun map<K, V>.set(mutate self, key: K, value: V): self
    builtin

/// Sets an element and returns the previous element at that key. If no previous, isFound = false.
/// ```
/// val prev = m.setAndGetPrevious(k, 3);
/// if (prev.isFound) {
///     prev.loadValue()
/// }
/// ```
@pure
fun map<K, V>.setAndGetPrevious(mutate self, key: K, value: V): MapLookupResult<V>
    builtin

/// Sets an element only if the key already exists. Returns whether an element was replaced.
@pure
fun map<K, V>.replaceIfExists(mutate self, key: K, value: V): bool
    builtin

/// Sets an element only if the key already exists and returns the previous element at that key.
@pure
fun map<K, V>.replaceAndGetPrevious(mutate self, key: K, value: V): MapLookupResult<V>
    builtin

/// Sets an element only if the key does not exist. Returns whether an element was added.
@pure
fun map<K, V>.addIfNotExists(mutate self, key: K, value: V): bool
    builtin

/// Sets an element only if the key does not exist. If exists, returns an old value.
@pure
fun map<K, V>.addOrGetExisting(mutate self, key: K, value: V): MapLookupResult<V>
    builtin

/// Delete an element at the key. Returns whether an element was deleted. 
@pure
fun map<K, V>.delete(mutate self, key: K): bool
    builtin

/// Delete an element at the key and returns the deleted element. If not exists, isFound = false.
/// ```
/// val prev = m.deleteAndGetDeleted(k);
/// if (prev.isFound) {
///     prev.loadValue()
/// }
/// ```
@pure
fun map<K, V>.deleteAndGetDeleted(mutate self, key: K): MapLookupResult<V>
    builtin

/// Finds the first (minimal) element in a map. If key are integers, it's the minimal integer.
/// If keys are addresses or complex structures (represented as slices), it's lexicographically smallest.
/// For an empty map, just returns isFound = false.
/// Useful for iterating over a map:
/// ```
/// var r = m.findFirst();
/// while (r.isFound) {
///     // ... use r.getKey() and r.loadValue()
///     r = m.iterateNext(r)
/// }
/// ```
@pure
fun map<K, V>.findFirst(self): MapEntry<K, V>
    builtin

/// Finds the last (maximal) element in a map. If key are integers, it's the maximal integer.
/// If keys are addresses or complex structures (represented as slices), it's lexicographically largest.
/// For an empty map, just returns isFound = false.
/// Useful for iterating over a map:
/// ```
/// var r = m.findLast();
/// while (r.isFound) {
///     // ... use r.getKey() and r.loadValue()
///     r = m.iteratePrev(r)
/// }
/// ```
@pure
fun map<K, V>.findLast(self): MapEntry<K, V>
    builtin

/// Finds an element with key > pivotKey.
/// Don't forget to check `isFound` before using `getKey()` and `loadValue()` of the result.
@pure
fun map<K, V>.findKeyGreater(self, pivotKey: K): MapEntry<K, V>
    builtin

/// Finds an element with key >= pivotKey.
/// Don't forget to check `isFound` before using `getKey()` and `loadValue()` of the result.
@pure
fun map<K, V>.findKeyGreaterOrEqual(self, pivotKey: K): MapEntry<K, V>
    builtin

/// Finds an element with key < pivotKey.
/// Don't forget to check `isFound` before using `getKey()` and `loadValue()` of the result.
@pure
fun map<K, V>.findKeyLess(self, pivotKey: K): MapEntry<K, V>
    builtin

/// Finds an element with key <= pivotKey.
/// Don't forget to check `isFound` before using `getKey()` and `loadValue()` of the result.
@pure
fun map<K, V>.findKeyLessOrEqual(self, pivotKey: K): MapEntry<K, V>
    builtin

/// Iterate over a map in ascending order.
/// ```
/// // iterate for all keys >= 10 up to the end
/// var r = m.findKeyGreaterOrEqual(10);
/// while (r.isFound) {
///     // ... use r.getKey() and r.loadValue()
///     r = m.iterateNext(r)
/// }
/// ```
@pure
fun map<K, V>.iterateNext(self, current: MapEntry<K, V>): MapEntry<K, V>
    builtin

/// Iterate over a map in reverse order.
/// ```
/// // iterate for all keys < 10 down lo lowest
/// var r = m.findKeyLess(10);
/// while (r.isFound) {
///     // ... use r.getKey() and r.loadValue()
///     r = m.iteratePrev(r)
/// }
/// ```
@pure
fun map<K, V>.iteratePrev(self, current: MapEntry<K, V>): MapEntry<K, V>
    builtin

/// `MapLookupResult` is a return value of `map.get`, `map.setAndGetPrevious`, and similar.
/// Instead of returning a nullable value (that you'd check on null before usage),
/// this struct is returned (and you check `isFound` before usage).
/// ```
/// val r = m.get(key);
/// if (r.isFound) {
///     r.loadValue()    // unpacks a returned slice
/// }
/// ```
struct MapLookupResult<TValue> {
    private readonly rawSlice: slice?        // holds encoded value, present if isFound
    isFound: bool
}

@pure
fun MapLookupResult<TValue>.loadValue(self): TValue {
    // it's assumed that you check for `isFound` before calling `loadValue()`;
    // note that `assertEnd` is called to ensure no remaining data left besides TValue
    return TValue.fromSlice(self.rawSlice!, {
        assertEndAfterReading: true
    })
}

@pure
fun MapLookupResult<slice>.loadValue(self): slice {
    return self.rawSlice!
}

/// `MapEntry` is a return value of `map.findFirst`, `map.iterateNext`, and similar.
/// You should check for `isFound` before calling `getKey()` and `loadValue()`.
/// ```
/// var r = m.findFirst();
/// while (r.isFound) {
///     // ... use r.getKey() and r.loadValue()
///     r = m.iterateNext(r)
/// }
/// ```
struct MapEntry<K, V> {
    private readonly rawValue: slice?        // holds encoded value, present if isFound
    private readonly key: K
    isFound: bool
}

@pure
fun MapEntry<K, V>.getKey(self): K {
    // it's assumed that you check for `isFound` before calling `getKey()`;
    // (otherwise, it will contain an incorrect stack slot with `null` value, not K)
    return self.key
}

@pure
fun MapEntry<K, V>.loadValue(self): V {
    // it's assumed that you check for `isFound` before calling `loadValue()`;
    // note that `assertEnd` is inserted to ensure no remaining data left besides TValue
    return V.fromSlice(self.rawValue!, {
        assertEndAfterReading: true
    })
}

@pure
fun MapEntry<K, slice>.loadValue(self): slice {
    return self.rawValue!
}


/**
    Slice primitives: parsing cells.
    When you _load_ some data, you mutate the slice (shifting an internal pointer on the stack).
    When you _preload_ some data, you just get the result without mutating the slice.
 */

/// Compile-time function that converts a constant hex-encoded string to N/2 bytes.
/// ```
/// const v = stringHexToSlice("abcdef") // = slice with 3 bytes: 0xAB,0xCD,0xEF
/// ```
/// Note: `stringHexToSlice(slice_var)` does NOT work! It accepts a constant string and works at compile-time.
@pure
fun stringHexToSlice(constStringBytesHex: slice): slice
    builtin

/// Converts a cell into a slice.
@pure
fun cell.beginParse(self): slice
    asm "CTOS"

/// Checks if a slice is empty. If not, throws an exception with code 9.
fun slice.assertEnd(self): void
    asm "ENDS"

/// Loads the next reference from a slice.
@pure
fun slice.loadRef(mutate self): cell
    asm( -> 1 0) "LDREF"

/// Preloads the next reference from a slice.
@pure
fun slice.preloadRef(self): cell
    asm "PLDREF"

/// Loads a signed len-bit integer from a slice.
@pure
fun slice.loadInt(mutate self, len: int): int
    builtin

/// Loads an unsigned len-bit integer from a slice.
@pure
fun slice.loadUint(mutate self, len: int): int
    builtin

/// Loads the first `0 ≤ len ≤ 1023` bits from a slice.
@pure
fun slice.loadBits(mutate self, len: int): slice
    builtin

/// Preloads a signed len-bit integer from a slice.
@pure
fun slice.preloadInt(self, len: int): int
    builtin

/// Preloads an unsigned len-bit integer from a slice.
@pure
fun slice.preloadUint(self, len: int): int
    builtin

/// Preloads the first `0 ≤ len ≤ 1023` bits from a slice.
@pure
fun slice.preloadBits(self, len: int): slice
    builtin

/// Loads the serialized amount of nanotoncoins (any unsigned integer up to `2^120 - 1`).
@pure
fun slice.loadCoins(mutate self): coins
    asm( -> 1 0) "LDGRAMS"

/// Loads a boolean from a slice.
@pure
fun slice.loadBool(mutate self): bool
    asm( -> 1 0) "1 LDI"

/// Shifts a slice pointer to len bits forward.
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
@pure
fun slice.loadDict(mutate self): dict
    asm( -> 1 0) "LDDICT"

/// Preloads a dictionary (cell) from a slice.
@pure
fun slice.preloadDict(self): dict
    asm "PLDDICT"

/// Skip a dictionary from a slice.
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

/// Skips (Maybe ^Cell) in a slice.
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

/// Converts a builder into an ordinary cell.
@pure
fun builder.endCell(self): cell
    asm "ENDC"

/// Converts a builder into a slice.
/// The same as `b.endCell().beginParse()`
@pure
fun builder.toSlice(self): slice
    asm "BTOS"

/// Stores a reference to a cell into a builder.
@pure
fun builder.storeRef(mutate self, c: cell): self
    asm(c self) "STREF"

/// Stores a signed len-bit integer into a builder (`0 ≤ len ≤ 257`).
@pure
fun builder.storeInt(mutate self, x: int, len: int): self
    builtin

/// Stores an unsigned len-bit integer into a builder (`0 ≤ len ≤ 256`).
@pure
fun builder.storeUint(mutate self, x: int, len: int): self
    builtin

/// Stores a slice into a builder.
@pure
fun builder.storeSlice(mutate self, s: slice): self
    asm(s self) "STSLICE"

/// Stores an internal address into a builder.
@pure
fun builder.storeAddress(mutate self, addr: address): self
    asm(addr self) "STSTDADDR"

/// Stores an internal address or '00' if the parameter is `null`.
/// '00' (two zero bits) is a standard representation of a "missing" ("none") address.
@pure
fun builder.storeAddressOpt(mutate self, addrOrNull: address?): self
    asm(addrOrNull self) "STOPTSTDADDR"

/// Stores "any address" (internal/external/none) into a builder.
@pure
fun builder.storeAddressAny(mutate self, addrAny: any_address): self
    asm(addrAny self) "STSLICE"

/// Stores a "none address": '00' (two zero bits) is TL addr_none$00.
@pure
fun builder.storeAddressNone(mutate self): self
    asm "b{00} STSLICECONST"

/// Stores the amount of nanotoncoins into a builder.
@pure
fun builder.storeCoins(mutate self, x: coins): self
    builtin

/// Stores a boolean into a builder.
@pure
fun builder.storeBool(mutate self, x: bool): self
    builtin

/// Stores a low-level TVM dictionary (optional cell) into a builder.
/// In other words, if `c` is null, stores `0`; if `c` is not null, stores `1` and a reference to `c`.
@pure
fun builder.storeDict(mutate self, c: dict): self
    asm(c self) "STDICT"

/// Stores (Maybe ^Cell) into a builder.
/// In other words, if `c` is null, stores `0`; if `c` is not null, stores `1` and a reference to `c`.
@pure
fun builder.storeMaybeRef(mutate self, c: cell?): self
    asm(c self) "STOPTREF"

/// Concatenates two builders.
@pure
fun builder.storeBuilder(mutate self, from: builder): self
    asm(from self) "STB"


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
/// ```
/// address("EQCRDM9h4k3UJdOePPuyX40mCgA4vxge5Dc5vjBR8djbEKC5")
/// address("0:527964d55cfa6eb731f4bfc07e9d025098097ef8505519e853986279bd8400d8")
/// ```
/// Returns `address`, which can be stored in a struct, compared with `==`, etc.
@pure
fun address(stdAddress: slice): address
    builtin

/// Extracts workchain and hash from a standard (internal) address.
@pure
fun address.getWorkchainAndHash(self): (int8, uint256)
    asm "REWRITESTDADDR"

/// Extracts workchain from a standard (internal) address.
@pure
fun address.getWorkchain(self): int8
    asm "REWRITESTDADDR" "DROP"

/// Creates a slice representing "none external address" (TL addr_none$00 — two zero bits).
/// Note! Use this in combination with EXTERNAL addresses (e.g. for [createExternalLogMessage]).
/// To represent "internal or none" address (which you most likely need to), 
/// use `address?` (nullable) and `null` accordingly.
@pure
fun createAddressNone(): any_address
    asm "b{00} PUSHSLICE"

/// Returns if "any address" is "none".
/// In TL/B, it's addr_none$00.
@pure
fun any_address.isNone(self): bool
    asm "b{00} SDBEGINSQ" "NIP"

/// Returns if "any address" is a standard (internal) address: workchain + hash.
/// Then it can be safely cast to `address`. 
/// In TL/B, it's addr_std$10.
@pure
fun any_address.isInternal(self): bool
    asm "b{10} SDBEGINSQ" "NIP"

/// Returns if "any address" is an external address, used to communication with the outside world.
/// In TL/B, it's addr_extern$01.
@pure
fun any_address.isExternal(self): bool
    asm "b{01} SDBEGINSQ" "NIP"

/// Extracts workchain and hash from a standard (internal) address.
/// If the address is not internal, throws a cell deserialization exception.
@pure
fun any_address.getWorkchainAndHash(self): (int8, uint256)
    asm "REWRITESTDADDR"

/// Extracts workchain from a standard (internal) address.
/// If the address is not internal, throws a cell deserialization exception.
@pure
fun any_address.getWorkchain(self): int8
    asm "REWRITESTDADDR" "DROP"

/// Casts `any_address` to `address` checking that "any" is actually "internal".
/// E.g., `addr = myAny.castToInternal()` does the check and returns `address`.
/// To skip the runtime check, use unsafe type casting instead: `addr = myAny as address`.
@pure
fun any_address.castToInternal(self): address
    asm "DUP" "b{10} SDBEGINSQ" "NIP" "9 THROWIFNOT"

/// Checks whether two addresses are equal. Equivalent to `a == b`.
/// Deprecated! Left for smoother transition from FunC, where you used `slice` everywhere.
/// Use just `a == b` and `a != b` to compare addresses, don't use bitsEqual.
@pure
@deprecated("use `senderAddress == ownerAddress`, not `senderAddress.bitsEqual(ownerAddress)`")
fun address.bitsEqual(self, b: address): bool
    asm "SDEQ"

/// Loads a standard (internal) address (containing workchain + hash). In TL/B, it's `MsgAddressInt`.
/// If an address is incorrect or is not internal, throws a deserialization exception (code 9).
@pure
fun slice.loadAddress(mutate self): address
    asm( -> 1 0) "LDSTDADDR"

/// Loads a standard (internal) address or returns `null` if the input is "none address".
/// If an address is incorrect or is not internal/none, throws a deserialization exception (code 9).
/// '00' (two zero bits) is a standard representation of a "missing" ("none") address.
/// As a result, you get either `address` (for internal) or `null` (for none).
@pure
fun slice.loadAddressOpt(mutate self): address?
    asm( -> 1 0) "LDOPTSTDADDR"

/// Loads a valid "any address" (internal/external/none). In TL/B, it's `MsgAddress`.
@pure
fun slice.loadAddressAny(mutate self): any_address
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
/// For `reserveMode` consider constants above.
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
/// in the case of action fail - bounce transaction. No effect if SEND_MODE_IGNORE_ERRORS (+2) is used. [TVM UPGRADE 2023-07](https://docs.ton.org/learn/tvm-instructions/tvm-upgrade-2023-07#sending-messages)
const SEND_MODE_BOUNCE_ON_ACTION_FAIL = 16
/// mode = 32 means that the current account must be destroyed if its resulting balance is zero.
const SEND_MODE_DESTROY = 32
/// mode = 64 is used for messages that carry all the remaining value of the inbound message in addition to the value initially indicated in the new message.
const SEND_MODE_CARRY_ALL_REMAINING_MESSAGE_VALUE = 64
/// mode = 128 is used for messages that are to carry all the remaining balance of the current smart contract (instead of the value originally indicated in the message).
const SEND_MODE_CARRY_ALL_BALANCE = 128
/// do not create an action, only estimate fee. [TVM UPGRADE 2023-07](https://docs.ton.org/learn/tvm-instructions/tvm-upgrade-2023-07#sending-messages)
const SEND_MODE_ESTIMATE_FEE_ONLY = 1024
/// Other modes affect the fee calculation as follows:
/// +64 substitutes the entire balance of the incoming message as an outcoming value (slightly inaccurate, gas expenses that cannot be estimated before the computation is completed are not taken into account).
/// +128 substitutes the value of the entire balance of the contract before the start of the computation phase (slightly inaccurate, since gas expenses that cannot be estimated before the completion of the computation phase are not taken into account).

/// `ExtraCurrenciesMap` represents a dictionary of "extra currencies" other than TON coin.
/// They can be attached to a message (incoming and outgoing) and stored on contract's balance.
/// It's a dictionary `[currencyID => amount]` (amount is like `coins`, but encoded with a high precision). 
type ExtraCurrenciesMap = map<int32, varuint32>

/// `ContractState` is "code + data" of a contract.
/// Used in outgoing messages (StateInit) to initialize a destination contract.
struct ContractState {
    code: cell
    data: cell
}

/// `AddressShardingOptions` provides settings to calculate an address in another shard.
/// Consider [createMessage] and [address.calculateSameAddressInAnotherShard] for usage.
struct AddressShardingOptions {
    fixedPrefixLength: uint5        // shard depth, formerly splitDepth
    closeTo: address
}

/// `AutoDeployAddress` is a destination that initializes a receiver contract if it does not exist yet.
/// In order to do this, it contains StateInit — and calculates an address by a hash of StateInit.
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
/// You can also use it without sending a message. 
/// See [AutoDeployAddress.calculateAddress].
struct AutoDeployAddress {
    workchain: int8 = BASECHAIN
    stateInit: ContractState | cell
    toShard: AddressShardingOptions? = null
}

/// Deprecated, use [AutoDeployAddress.calculateAddress] instead.
/// Since late 2025 (TVM 12 has a cheap "builder-to-slice" instruction),
/// there is no need to use this function.
/// Historically its purpose was to avoid creating a cell and to return `builder` with a valid address.
@pure
@deprecated("use calculateAddress instead")
fun AutoDeployAddress.buildAddress(self): builder
    builtin

/// Constructs an address that a deployed contract will have.
/// For example, from a jetton minter, you want to calculate an address of a jetton wallet:
/// ```
/// val jwDeployed = calcDeployedJettonWallet(...);
/// val jwAddress = jwDeployed.calculateAddress();
/// ```
/// Just instead of `dest` for [createMessage], you use it is to calculate a jetton/nft address.
@pure
fun AutoDeployAddress.calculateAddress(self): address
    builtin

/// Checks that an address matches a deployed contract.
/// For example, from a jetton minter, you're checking whether a message is from a jetton wallet:
/// ```
/// val jwDeployed = calcDeployedJettonWallet(...);
/// val fromJW = jwDeployed.addressMatches(senderAddress);
/// ```
/// Just instead of `dest` for [createMessage], you use it is to check whether a sender is a jetton/nft.
/// Deprecated since late 2025 (TVM 12 with a cheap "builder-to-slice" instruction), to be removed later.
@pure
@deprecated("Use `a == jwDeployed.calculateAddress()` instead of `jwDeployed.addressMatches(a)")
fun AutoDeployAddress.addressMatches(self, addr: address): bool
    builtin

/// `BounceMode` represents behavior of an outgoing message when a receiver failed to handle it.
/// If you specify to bounce a message, don't forget about the `onBouncedMessage` entrypoint.
/// ```
/// val outMsg = createMessage({
///     bounce: BounceMode.Only256BitsOfBody,
///     ...
/// });
/// 
/// fun onBouncedMessage(in: InMessageBounced) {
///     // in.bouncedBody is populated depending on BounceMode
///     // for instance, "Only256BitsOfBody" means 
///     // "bounced prefix 0xFFFFFFFF" and 256 bits of data
///     in.bouncedBody.skipBouncedPrefix();
///     val originalOpcode = in.bouncedBody.loadUint(32);
///     ...
/// }
/// ```
enum BounceMode {
    NoBounce                // a message will just disappear on error
    Only256BitsOfBody       // `in.bouncedBody` will be "0xFFFFFFFF" and the first 256 bits of outgoing body (most cheap)
    RichBounce              // `in.bouncedBody` will be struct RichBounceBody (most expensive, but allows accessing all data sent)
    RichBounceOnlyRootCell  // `in.bouncedBody` will be struct RichBounceBody without refs in `originalBody`
}

/// Before TVM-12, `bounce` was only true or false.
/// Now, use `enum BounceMode` instead. "true" behaves like `Only256BitsOfBody`.
@deprecated("use `BounceMode.NoBounce` instead of `false`")
type OldBounceMode = bool

/// Options for creating an outgoing message.
/// Consider [createMessage] for examples.
struct CreateMessageOptions<TBody = void> {
    /// whether a message will bounce back on error, and what piece of outgoing `body` it will contain
    bounce: BounceMode | OldBounceMode
    /// message value: attached tons (or tons + extra currencies)
    value: coins | (coins, ExtraCurrenciesMap)
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
/// ```
/// val reply = createMessage({
///     bounce: BounceMode.NoBounce,
///     value: ton("0.05"),
///     dest: senderAddress,
///     body: RequestedInfo { ... }    // note: no toCell! just pass an object
/// });
/// reply.send(SEND_MODE_REGULAR);
/// ```
///
/// Hint: don't call `body.toCell()`, pass `body` directly!
///       (if body is small, it will be inlined without an expensive cell creation)
///       (if body is large, the compiler will automatically wrap it into a cell)
/// If you need an empty body, just miss the field `body`, fill other 3 fields.
@pure
fun createMessage<TBody>(options: CreateMessageOptions<TBody>): OutMessage
    builtin


/// `ExtOutLogBucket` is a variant of a custom external address for emitting logs "to the outer world".
/// It includes some "topic" (arbitrary number), that determines the format of the message body.
/// For example, you emit "deposit event" (reserving topic "deposit" = 123):
/// ```
/// dest: ExtOutLogBucket { topic: 123 }, 
/// body: DepositData { ... }
/// ```
/// and external indexers can index your emitted logs by destination address without parsing body.
/// Serialization details: '01' (addr_extern) + 256 (len) + 0x00 (prefix) + 248 bits = 267 in total
struct (0x00) ExtOutLogBucket {
    topic: uint248 | bits248
}

/// Options for creating an external outgoing message.
/// Consider [createExternalLogMessage] for examples.
struct CreateExternalLogMessageOptions<TBody = void> {
    /// destination is either an external address or a pattern to calculate it
    dest: any_address       // either some valid external/none address (not internal!)
        | builder           // ... or a manually constructed builder with a valid external address
        | ExtOutLogBucket   // ... or encode topic/eventID in destination
    /// body is any serializable object (or just miss this field for empty body)
    body: TBody
}

/// Creates an external message (`OutMessage`) — a well-formatted message cell.
/// Typically, you just send it. In advanced scenarios, you can estimate fees or even postpone sending.
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

/// `UnsafeBodyNoRef` is used to prevent default behavior: when message body is potentially large,
/// it's packed into a separate ref. Wrapping body with this struct tells the compiler:
/// "inline message body in-place, I guarantee it will fit".
/// ```
/// struct ProbablyLarge { a: (coins, coins, coins, coins, coins) }
///
/// val contents: ProbablyLarge = { ... };  // you are sure: values are small
/// createMessage({
///     // body: contents,           // by default, will be a ref (max 620 bits)
///     body: UnsafeBodyNoRef {
///         forceInline: contents,   // but it forces the compiler to inline it
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


/// `StateInit` is a "canonical TL/B representation from block.tlb" of a contract initial state.
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
/// ```
/// val addrHash = StateInit.calcHashCodeData(codeCell, dataCell);
/// createMessage({
///     dest: (workchain, addrHash),
///     ...
/// ```
@pure
fun StateInit.calcHashCodeData(code: cell, data: cell): uint256 asm (data code)
    "NEWC"
    "b{00110} STSLICECONST"
    "STREF"
    "STREF"
    "HASHBU"

/// Calculates a hash of StateInit if fixedPrefixLength+code+data are set.
@pure
fun StateInit.calcHashPrefixCodeData(fixedPrefixLength: uint5, code: cell, data: cell): uint256 asm (data code fixedPrefixLength)
    "NEWC"
    "b{1} STSLICECONST"
    "5 STU"
    "b{0110} STSLICECONST"
    "STREF"
    "STREF"
    "HASHBU"


/// Deprecated, use [address.calculateSameAddressInAnotherShard] instead.
/// Since late 2025 (TVM 12 has a cheap "builder-to-slice" instruction),
/// there is no need to use this function.
/// Historically its purpose was to avoid creating a cell and to return `builder` with a valid address.
@pure
@deprecated("use calculateSameAddressInAnotherShard instead")
fun address.buildSameAddressInAnotherShard(self, options: AddressShardingOptions): builder
    builtin

/// Given an internal address A="aaaa...a" returns "bbaa...a" (D bits from address B, 256-D from A).
///
/// Example for fixedPrefixLength (shard depth) = 8:
/// ```
/// self (A)     | aaaaaaaaaaa...aaa |
/// closeTo (B)  | 01010101bbb...bbb |   shardPrefix = 01010101 (depth 8)
/// result       | 01010101aaa...aaa |   address of A in same shard as B
/// ```
/// More precisely, self (input) is 267 bits: '100' (std addr no anycast) + workchainA + "aaaa...a".
/// The result is also 267 bits: '100' + workchainB + "bb" (D bits) + "aa...a" (256-D bits).
@pure
fun address.calculateSameAddressInAnotherShard(self, options: AddressShardingOptions): address
    builtin

/// Converts a builder containing a valid address to `address` (slice under the hood).
/// Deprecated! Since late 2025 (TVM 12 has a cheap "builder-to-slice" instruction),
/// all compiler built-ins return `address` directly, not `builder`.
@pure
@deprecated("prefer using `address` everywhere, avoid using `builder` and manual address construction")
fun address.fromValidBuilder(b: builder): address
    asm "ENDC" "CTOS"

/// Constructs an internal address (267 bits) from workchain and hash.
/// Hint: if you just want to send a message, don't call this function, pass workchain+hash directly:
/// ```
/// createMessage({
///     dest: (workchain, hash),   // a pair can be passed instead of `address`
///     ...                        // so, don't call this function in such a case
/// ```
fun address.fromWorkchainAndHash(workchain: int8, hash: uint256): address 
    asm (hash workchain)
    "NEWC"
    "b{100} STSLICECONST"       // addr_std$10 + 0 (no anycast)
    "8 STI"
    "256 STU"
    "BTOS"


/// Sends a raw message — a correctly serialized TL object `Message X`.
/// In practice, you'll use a high-level wrapper [createMessage].
fun sendRawMessage(msg: cell, mode: int): void
    asm "SENDRAWMSG"



/**
    Receiving and handling messages.
*/

/// `InMessage` is an input for `onInternalMessage` — when your contract accepts a non-bounced message.
/// Internally, some data exist on the stack, some can be acquired with TVM instructions.
/// The compiler replaces `in.someField` and gets a value correctly.
/// ```
/// fun onInternalMessage(in: InMessage) {
///     in.senderAddress   // actually calls `INMSG_SRC` asm instruction
///     in.body            // actually gets from a TVM stack
/// ```
struct InMessage {
    senderAddress: address          // an internal address from which the message arrived
    valueCoins: coins               // ton amount attached to an incoming message
    valueExtra: ExtraCurrenciesMap  // extra currencies attached to an incoming message
    originalForwardFee: coins       // fee that was paid by the sender
    createdLt: uint64               // logical time when a message was created
    createdAt: uint32               // unixtime when a message was created
    body: slice                     // message body, parse it with `lazy AllowedMsg.fromSlice(in.body)`
}

/// `InMessageBounced` is an input for `onBouncedMessage`.
/// Very similar to a non-bounced input [InMessage].
/// Note that `bouncedBody` is populated depending on [BounceMode] specified for [createMessage].
/// ```
/// fun onBouncedMessage(in: InMessageBounced) {
///     // in.bouncedBody is populated depending on BounceMode
///     // for instance, "Only256BitsOfBody" means 
///     // "bounced prefix 0xFFFFFFFF" and 256 bits of data
///     in.bouncedBody.skipBouncedPrefix();
///     val originalOpcode = in.bouncedBody.loadUint(32);
/// ```
struct InMessageBounced {
    senderAddress: address          // an internal address from which the message was bounced
    valueCoins: coins               // ton amount attached to a message
    valueExtra: ExtraCurrenciesMap  // extra currencies attached to a message
    originalForwardFee: coins       // comission that the sender has payed to send this message
    createdLt: uint64               // logical time when a message was created (and bounced)
    createdAt: uint32               // unixtime when a message was created (and bounced)
    bouncedBody: slice              // populated depending on `BounceMode` for `createMessage`
}

/// When you use [createMessage] with "rich bounce mode", and a message is bounced,
/// then `in.bouncedBody` inside `onBouncedMessage` has this format.
/// ```
/// fun onBouncedMessage(in: InMessageBounced) {
///     // suppose you ALWAYS use BounceMode.RichBounce in createMessage
///     // so you parse ALL bounces as follows
///     val rich = lazy RichBounceBody.fromSlice(in.bouncedBody);
///     // handle rich.originalBody, probably with lazy match
///     // use rich.xxx to get exitCode, gasUsed, and so on
/// ```
struct (0xfffffffe) RichBounceBody {
    originalBody: cell                              // a cell with original body, but for `RichBounceOnlyRootCell` without refs (cheaper)
    originalInfo: Cell<RichBounceOriginalMsgInfo>   // some fields of an original (outgoing) message
    bouncedByPhase: uint8                           // see docs
    exitCode: int32                                 // exception code from `throw` or TVM internals
    computePhase: RichBounceComputePhaseInfo?       // filled if compute phase executed
}

/// Describes compute phase details of a bounced message. 
struct RichBounceComputePhaseInfo {
    gasUsed: uint32
    vmSteps: uint32
}

/// Describes original outgoing message fields (besides body) of a bounced message.
/// ```
/// createMessage({
///     bounce: BounceMode.RichBounce,
///     value: ton("0.05")   // it will be `valueCoins` if this message is bounced
/// ```
struct RichBounceOriginalMsgInfo {
    valueCoins: coins
    valueExtra: ExtraCurrenciesMap
    createdLt: uint64
    createdAt: uint32
}

/// Skip 0xFFFFFFFF prefix (when a message is bounced).
/// Used when parsing `in.bounceBody` for "not rich" `BounceMode.Only256BitsOfBody` bounces.
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


