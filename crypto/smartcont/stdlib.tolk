// Standard library for Tolk
// (initially copied from stdlib.fc)
//
tolk 0.6

/*
    This file is part of TON Tolk Standard Library.

    Tolk Standard Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Tolk Standard Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.
*/

/*
  # Tuple manipulation primitives
  The names and the types are mostly self-explaining.

  Note that currently values of atomic type `tuple` can't be cast to composite tuple type (e.g. `[int, cell]`)
  and vise versa.
*/

/***
  # Lisp-style lists

  Lists can be represented as nested 2-elements tuples.
  Empty list is conventionally represented as TVM `null` value.
  For example, tuple `(1, (2, (3, null)))` represents list `[1, 2, 3]`. Elements of a list can be of different types.
*/

/// Adds an element to the beginning of lisp-style list.
@pure
fun cons<X>(head: X, tail: tuple): tuple
    asm "CONS";

/// Extracts the head and the tail of lisp-style list.
@pure
fun uncons<X>(list: tuple): (X, tuple)
    asm "UNCONS";

/// Extracts the tail and the head of lisp-style list.
@pure
fun list_next<X>(list: tuple): (tuple, X)
    asm( -> 1 0) "UNCONS";

/// Returns the head of lisp-style list.
@pure
fun car<X>(list: tuple): X
    asm "CAR";

/// Returns the tail of lisp-style list.
@pure
fun cdr(list: tuple): tuple
    asm "CDR";

/// Creates tuple with zero elements.
@pure
fun empty_tuple(): tuple
    asm "NIL";

/// Appends a value `x` to a `Tuple t = (x1, ..., xn)`, but only if the resulting `Tuple t' = (x1, ..., xn, x)`
/// is of length at most 255. Otherwise throws a type check exception.
@pure
fun tpush<X>(t: tuple, value: X): tuple
    asm "TPUSH";

@pure
fun ~tpush<X>(t: tuple, value: X): (tuple, ())
    asm "TPUSH";

/// Creates a tuple of length one with given argument as element.
@pure
fun single<X>(x: X): [X]
    asm "SINGLE";

/// Unpacks a tuple of length one
@pure
fun unsingle<X>(t: [X]): X
    asm "UNSINGLE";

/// Creates a tuple of length two with given arguments as elements.
@pure
fun pair<X, Y>(x: X, y: Y): [X, Y]
    asm "PAIR";

/// Unpacks a tuple of length two
@pure
fun unpair<X, Y>(t: [X, Y]): (X, Y)
    asm "UNPAIR";

/// Creates a tuple of length three with given arguments as elements.
@pure
fun triple<X, Y, Z>(x: X, y: Y, z: Z): [X, Y, Z]
    asm "TRIPLE";

/// Unpacks a tuple of length three
@pure
fun untriple<X, Y, Z>(t: [X, Y, Z]): (X, Y, Z)
    asm "UNTRIPLE";

/// Creates a tuple of length four with given arguments as elements.
@pure
fun tuple4<X, Y, Z, W>(x: X, y: Y, z: Z, w: W): [X, Y, Z, W]
    asm "4 TUPLE";

/// Unpacks a tuple of length four
@pure
fun untuple4<X, Y, Z, W>(t: [X, Y, Z, W]): (X, Y, Z, W)
    asm "4 UNTUPLE";

/// Returns the first element of a tuple (with unknown element types).
@pure
fun first<X>(t: tuple): X
    asm "FIRST";

/// Returns the second element of a tuple (with unknown element types).
@pure
fun second<X>(t: tuple): X
    asm "SECOND";

/// Returns the third element of a tuple (with unknown element types).
@pure
fun third<X>(t: tuple): X
    asm "THIRD";

/// Returns the fourth element of a tuple (with unknown element types).
@pure
fun fourth<X>(t: tuple): X
    asm "3 INDEX";

/// Returns the [`index`]-th element of tuple [`t`].
@pure
fun at<X>(t: tuple, index: int): X
    builtin;

/// Returns the first element of a pair tuple.
@pure
fun pair_first<X, Y>(p: [X, Y]): X
    asm "FIRST";

/// Returns the second element of a pair tuple.
@pure
fun pair_second<X, Y>(p: [X, Y]): Y
    asm "SECOND";

/// Returns the first element of a triple tuple.
@pure
fun triple_first<X, Y, Z>(p: [X, Y, Z]): X
    asm "FIRST";

/// Returns the second element of a triple tuple.
@pure
fun triple_second<X, Y, Z>(p: [X, Y, Z]): Y
    asm "SECOND";

/// Returns the third element of a triple tuple.
@pure
fun triple_third<X, Y, Z>(p: [X, Y, Z]): Z
    asm "THIRD";


/// Moves a variable [x] to the top of the stack.
@pure
fun touch<X>(x: X): X
    builtin;

/// Moves a variable [x] to the top of the stack.
@pure
fun ~touch<X>(x: X): (X, ())
    builtin;

/// Mark a variable as used, such that the code which produced it won't be deleted even if it is not impure.
fun ~impure_touch<X>(x: X): (X, ())
    asm "NOP";



/// Returns the current Unix time as an Integer
@pure
fun now(): int
    asm "NOW";

/// Returns the internal address of the current smart contract as a Slice with a `MsgAddressInt`.
/// If necessary, it can be parsed further using primitives such as [parse_std_addr].
@pure
fun my_address(): slice
    asm "MYADDR";

/// Returns the balance of the smart contract as a tuple consisting of an int
/// (balance in nanotoncoins) and a `cell`
/// (a dictionary with 32-bit keys representing the balance of "extra currencies")
/// at the start of Computation Phase.
/// Note that RAW primitives such as [send_raw_message] do not update this field.
@pure
fun get_balance(): [int, cell]
    asm "BALANCE";

/// Returns the logical time of the current transaction.
@pure
fun cur_lt(): int
    asm "LTIME";

/// Returns the starting logical time of the current block.
@pure
fun block_lt(): int
    asm "BLOCKLT";

/// Computes the representation hash of a `cell` [c] and returns it as a 256-bit unsigned integer `x`.
/// Useful for signing and checking signatures of arbitrary entities represented by a tree of cells.
@pure
fun cell_hash(c: cell): int
    asm "HASHCU";

/// Computes the hash of a `slice s` and returns it as a 256-bit unsigned integer `x`.
/// The result is the same as if an ordinary cell containing only data and references from `s` had been created
/// and its hash computed by [cell_hash].
@pure
fun slice_hash(s: slice): int
    asm "HASHSU";

/// Computes sha256 of the data bits of `slice` [s]. If the bit length of `s` is not divisible by eight,
/// throws a cell underflow exception. The hash value is returned as a 256-bit unsigned integer `x`.
@pure
fun string_hash(s: slice): int
    asm "SHA256U";

/***
  # Signature checks
*/

/// Checks the Ed25519-`signature` of a `hash` (a 256-bit unsigned integer, usually computed as the hash of some data)
/// using [public_key] (also represented by a 256-bit unsigned integer).
/// The signature must contain at least 512 data bits; only the first 512 bits are used.
/// The result is `−1` if the signature is valid, `0` otherwise.
/// Note that `CHKSIGNU` creates a 256-bit slice with the hash and calls `CHKSIGNS`.
/// That is, if [hash] is computed as the hash of some data, these data are hashed twice,
/// the second hashing occurring inside `CHKSIGNS`.
@pure
fun check_signature(hash: int, signature: slice, public_key: int): int
    asm "CHKSIGNU";

/// Checks whether [signature] is a valid Ed25519-signature of the data portion of `slice data` using `public_key`,
/// similarly to [check_signature].
/// If the bit length of [data] is not divisible by eight, throws a cell underflow exception.
/// The verification of Ed25519 signatures is the standard one,
/// with sha256 used to reduce [data] to the 256-bit number that is actually signed.
@pure
fun check_data_signature(data: slice, signature: slice, public_key: int): int
    asm "CHKSIGNS";

/***
  # Computation of boc size
  The primitives below may be useful for computing storage fees of user-provided data.
*/

/// A non-quiet version of [compute_data_size?] that throws a cell overflow exception (`8`) on failure.
fun compute_data_size(c: cell, max_cells: int): (int, int, int)
    asm "CDATASIZE";

/// A non-quiet version of [slice_compute_data_size?] that throws a cell overflow exception (`8`) on failure.
fun slice_compute_data_size(s: slice, max_cells: int): (int, int, int)
    asm "SDATASIZE";

/// Returns `(x, y, z, -1)` or `(null, null, null, 0)`.
/// Recursively computes the count of distinct cells `x`, data bits `y`, and cell references `z`
/// in the DAG rooted at `cell` [c], effectively returning the total storage used by this DAG taking into account
/// the identification of equal cells.
/// The values of `x`, `y`, and `z` are computed by a depth-first traversal of this DAG,
/// with a hash table of visited cell hashes used to prevent visits of already-visited cells.
/// The total count of visited cells `x` cannot exceed non-negative [max_cells];
/// otherwise the computation is aborted before visiting the `(max_cells + 1)`-st cell and
/// a zero flag is returned to indicate failure. If [c] is `null`, returns `x = y = z = 0`.
@pure
fun compute_data_size?(c: cell, max_cells: int): (int, int, int, int)
    asm "CDATASIZEQ NULLSWAPIFNOT2 NULLSWAPIFNOT";

/// Similar to [compute_data_size?], but accepting a `slice` [s] instead of a `cell`.
/// The returned value of `x` does not take into account the cell that contains the `slice` [s] itself;
/// however, the data bits and the cell references of [s] are accounted for in `y` and `z`.
@pure
fun slice_compute_data_size?(s: slice, max_cells: int): (int, int, int, int)
    asm "SDATASIZEQ NULLSWAPIFNOT2 NULLSWAPIFNOT";

/***
  # Debug primitives
  Only works for local TVM execution with debug level verbosity
*/

/// Dump a variable [x] to the debug log.
fun ~dump<X>(x: X): (X, ())
    builtin;

/// Dump a string [x] to the debug log.
fun ~strdump<X>(x: X): (X, ())
    builtin;

/// Dumps the stack (at most the top 255 values) and shows the total stack depth.
fun dump_stack(): void
    asm "DUMPSTK";

/***
  # Persistent storage save and load
*/

/// Returns the persistent contract storage cell. It can be parsed or modified with slice and builder primitives later.
@pure
fun get_data(): cell
    asm "c4 PUSH";

/// Sets `cell` [c] as persistent contract data. You can update persistent contract storage with this primitive.
fun set_data(c: cell): void
    asm "c4 POP";

/***
  # Continuation primitives
*/
/// Usually `c3` has a continuation initialized by the whole code of the contract. It is used for function calls.
/// The primitive returns the current value of `c3`.
@pure
fun get_c3(): continuation
    asm "c3 PUSH";

/// Updates the current value of `c3`. Usually, it is used for updating smart contract code in run-time.
/// Note that after execution of this primitive the current code
/// (and the stack of recursive function calls) won't change,
/// but any other function call will use a function from the new code.
fun set_c3(c: continuation): void
    asm "c3 POP";

/// Transforms a `slice` [s] into a simple ordinary continuation `c`, with `c.code = s` and an empty stack and savelist.
@pure
fun bless(s: slice): continuation
    asm "BLESS";

/***
  # Gas related primitives
*/

/// Sets current gas limit `gl` to its maximal allowed value `gm`, and resets the gas credit `gc` to zero,
/// decreasing the value of `gr` by `gc` in the process.
/// In other words, the current smart contract agrees to buy some gas to finish the current transaction.
/// This action is required to process external messages, which bring no value (hence no gas) with themselves.
///
/// For more details check [accept_message effects](https://ton.org/docs/#/smart-contracts/accept).
fun accept_message(): void
    asm "ACCEPT";

/// Sets current gas limit `gl` to the minimum of limit and `gm`, and resets the gas credit `gc` to zero.
/// If the gas consumed so far (including the present instruction) exceeds the resulting value of `gl`,
/// an (unhandled) out of gas exception is thrown before setting new gas limits.
/// Notice that [set_gas_limit] with an argument `limit ≥ 2^63 − 1` is equivalent to [accept_message].
fun set_gas_limit(limit: int): void
    asm "SETGASLIMIT";

/// Commits the current state of registers `c4` (“persistent data”) and `c5` (“actions”)
/// so that the current execution is considered “successful” with the saved values even if an exception
/// in Computation Phase is thrown later.
fun commit(): void
    asm "COMMIT";

/// Computes the amount of gas that can be bought for `amount` nanoTONs,
/// and sets `gl` accordingly in the same way as [set_gas_limit].
fun buy_gas(amount: int): void
    asm "BUYGAS";

/// Computes the minimum of two integers [x] and [y].
@pure
fun min(x: int, y: int): int
    asm "MIN";

/// Computes the maximum of two integers [x] and [y].
@pure
fun max(x: int, y: int): int
    asm "MAX";

/// Sorts two integers.
@pure
fun minmax(x: int, y: int): (int, int)
    asm "MINMAX";

/// Computes the absolute value of an integer [x].
@pure
fun abs(x: int): int
    asm "ABS";

/// Computes the quotient and remainder of [x] / [y]. Example: divmod(112,3) = (37,1)
@pure
fun divmod(x: int, y: int): (int, int)
    builtin;

/// Computes the remainder and quotient of [x] / [y]. Example: moddiv(112,3) = (1,37)
@pure
fun moddiv(x: int, y: int): (int, int)
    builtin;

/// Computes multiple-then-divide: floor([x] * [y] / [z]).
/// The intermediate result is stored in a 513-bit integer to prevent precision loss.
@pure
fun muldiv(x: int, y: int, z: int): int
    builtin;

/// Similar to `muldiv`, but rounds the result: round([x] * [y] / [z]).
@pure
fun muldivr(x: int, y: int, z: int): int
    builtin;

/// Similar to `muldiv`, but ceils the result: ceil([x] * [y] / [z]).
@pure
fun muldivc(x: int, y: int, z: int): int
    builtin;

/// Computes the quotient and remainder of ([x] * [y] / [z]). Example: muldivmod(112,3,10) = (33,6)
@pure
fun muldivmod(x: int, y: int, z: int): (int, int)
    builtin;

/***
  # Slice primitives

  It is said that a primitive _loads_ some data,
  if it returns the data and the remainder of the slice
  (so it can also be used as modifying method).

  It is said that a primitive _preloads_ some data, if it returns only the data
  (it can be used as non-modifying method).

  Unless otherwise stated, loading and preloading primitives read the data from a prefix of the slice.
*/


/// Converts a `cell` [c] into a `slice`. Notice that [c] must be either an ordinary cell,
/// or an exotic cell (see [TVM.pdf](https://ton-blockchain.github.io/docs/tvm.pdf), 3.1.2)
/// which is automatically loaded to yield an ordinary cell `c'`, converted into a `slice` afterwards.
@pure
fun begin_parse(c: cell): slice
    asm "CTOS";

/// Checks if [s] is empty. If not, throws an exception.
fun end_parse(s: slice): void
    asm "ENDS";

/// Loads the first reference from the slice.
@pure
fun load_ref(s: slice): (slice, cell)
    asm( -> 1 0) "LDREF";

/// Preloads the first reference from the slice.
@pure
fun preload_ref(s: slice): cell
    asm "PLDREF";

/// Loads a signed [len]-bit integer from a slice [s].
@pure
fun load_int(s: slice, len: int): (slice, int)
    builtin;

/// Loads an unsigned [len]-bit integer from a slice [s].
@pure
fun load_uint(s: slice, len: int): (slice, int)
    builtin;

/// Preloads a signed [len]-bit integer from a slice [s].
@pure
fun preload_int(s: slice, len: int): int
    builtin;

/// Preloads an unsigned [len]-bit integer from a slice [s].
@pure
fun preload_uint(s: slice, len: int): int
    builtin;

/// Loads the first `0 ≤ len ≤ 1023` bits from slice [s] into a separate slice `s''`.
@pure
fun load_bits(s: slice, len: int): (slice, slice)
    builtin;

/// Preloads the first `0 ≤ len ≤ 1023` bits from slice [s] into a separate slice `s''`.
@pure
fun preload_bits(s: slice, len: int): slice
    builtin;

/// Loads serialized amount of TonCoins (any unsigned integer up to `2^120 - 1`).
@pure
fun load_grams(s: slice): (slice, int)
    asm( -> 1 0) "LDGRAMS";

@pure
fun load_coins(s: slice): (slice, int)
    asm( -> 1 0) "LDGRAMS";

/// Returns all but the first `0 ≤ len ≤ 1023` bits of `slice` [s].
@pure
fun skip_bits(s: slice, len: int): slice
    asm "SDSKIPFIRST";

@pure
fun ~skip_bits(s: slice, len: int): (slice, ())
    asm "SDSKIPFIRST";

/// Returns the first `0 ≤ len ≤ 1023` bits of `slice` [s].
@pure
fun first_bits(s: slice, len: int): slice
    asm "SDCUTFIRST";

/// Returns all but the last `0 ≤ len ≤ 1023` bits of `slice` [s].
@pure
fun skip_last_bits(s: slice, len: int): slice
    asm "SDSKIPLAST";
@pure
fun ~skip_last_bits(s: slice, len: int): (slice, ())
    asm "SDSKIPLAST";

/// Returns the last `0 ≤ len ≤ 1023` bits of `slice` [s].
@pure
fun slice_last(s: slice, len: int): slice
    asm "SDCUTLAST";

/// Loads a dictionary `D` (HashMapE) from `slice` [s].
/// (returns `null` if `nothing` constructor is used).
@pure
fun load_dict(s: slice): (slice, cell)
    asm( -> 1 0) "LDDICT";

/// Preloads a dictionary `D` from `slice` [s].
@pure
fun preload_dict(s: slice): cell
    asm "PLDDICT";

/// Loads a dictionary as [load_dict], but returns only the remainder of the slice.
@pure
fun skip_dict(s: slice): slice
    asm "SKIPDICT";

@pure
fun ~skip_dict(s: slice): (slice, ())
    asm "SKIPDICT";

/// Loads (Maybe ^Cell) from `slice` [s].
/// In other words loads 1 bit and if it is true
/// loads first ref and return it with slice remainder
/// otherwise returns `null` and slice remainder
@pure
fun load_maybe_ref(s: slice): (slice, cell)
    asm( -> 1 0) "LDOPTREF";

/// Preloads (Maybe ^Cell) from `slice` [s].
@pure
fun preload_maybe_ref(s: slice): cell
    asm "PLDOPTREF";


/// Returns the depth of `cell` [c].
/// If [c] has no references, then return `0`;
/// otherwise the returned value is one plus the maximum of depths of cells referred to from [c].
/// If [c] is a `null` instead of a cell, returns zero.
@pure
fun cell_depth(c: cell): int
    asm "CDEPTH";


/***
  # Slice size primitives
*/

/// Returns the number of references in `slice` [s].
@pure
fun slice_refs(s: slice): int
    asm "SREFS";

/// Returns the number of data bits in `slice` [s].
@pure
fun slice_bits(s: slice): int
    asm "SBITS";

/// Returns both the number of data bits and the number of references in `slice` [s].
@pure
fun slice_bits_refs(s: slice): (int, int)
    asm "SBITREFS";

/// Checks whether a `slice` [s] is empty (i.e., contains no bits of data and no cell references).
@pure
fun slice_empty?(s: slice): int
    asm "SEMPTY";

/// Checks whether `slice` [s] has no bits of data.
@pure
fun slice_data_empty?(s: slice): int
    asm "SDEMPTY";

/// Checks whether `slice` [s] has no references.
@pure
fun slice_refs_empty?(s: slice): int
    asm "SREMPTY";

/// Returns the depth of `slice` [s].
/// If [s] has no references, then returns `0`;
/// otherwise the returned value is one plus the maximum of depths of cells referred to from [s].
@pure
fun slice_depth(s: slice): int
    asm "SDEPTH";

/***
  # Builder size primitives
*/

/// Returns the number of cell references already stored in `builder` [b]
@pure
fun builder_refs(b: builder): int
    asm "BREFS";

/// Returns the number of data bits already stored in `builder` [b].
@pure
fun builder_bits(b: builder): int
    asm "BBITS";

/// Returns the depth of `builder` [b].
/// If no cell references are stored in [b], then returns 0;
/// otherwise the returned value is one plus the maximum of depths of cells referred to from [b].
@pure
fun builder_depth(b: builder): int
    asm "BDEPTH";

/***
  # Builder primitives
  It is said that a primitive _stores_ a value `x` into a builder `b`
  if it returns a modified version of the builder `b'` with the value `x` stored at the end of it.
  It can be used as non-modifying method.

  All the primitives below first check whether there is enough space in the `builder`,
  and only then check the range of the value being serialized.
*/

/// Creates a new empty `builder`.
@pure
fun begin_cell(): builder
    asm "NEWC";

/// Converts a `builder` into an ordinary `cell`.
@pure
fun end_cell(b: builder): cell
    asm "ENDC";

/// Stores a reference to `cell` [c] into `builder` [b].
@pure
fun store_ref(b: builder, c: cell): builder
    asm(c b) "STREF";

/// Stores an unsigned [len]-bit integer `x` into `b` for `0 ≤ len ≤ 256`.
@pure
fun store_uint(b: builder, x: int, len: int): builder
    builtin;

/// Stores a signed [len]-bit integer `x` into `b` for `0 ≤ len ≤ 257`.
@pure
fun store_int(b: builder, x: int, len: int): builder
    builtin;

/// Stores `slice` [s] into `builder` [b].
@pure
fun store_slice(b: builder, s: slice): builder
    asm "STSLICER";

/// Stores (serializes) an integer [x] in the range `0..2^120 − 1` into `builder` [b].
/// The serialization of [x] consists of a 4-bit unsigned big-endian integer `l`,
/// which is the smallest integer `l ≥ 0`, such that `x < 2^8l`,
/// followed by an `8l`-bit unsigned big-endian representation of [x].
/// If [x] does not belong to the supported range, a range check exception is thrown.
///
/// Store amounts of TonCoins to the builder as VarUInteger 16
@pure
fun store_grams(b: builder, x: int): builder
    asm "STGRAMS";

@pure
fun store_coins(b: builder, x: int): builder
    asm "STGRAMS";

/// Stores dictionary `D` represented by `cell` [c] or `null` into `builder` [b].
/// In other words, stores a `1`-bit and a reference to [c] if [c] is not `null` and `0`-bit otherwise.
@pure
fun store_dict(b: builder, c: cell): builder
    asm(c b) "STDICT";

/// Stores (Maybe ^Cell) to builder:
/// if cell is null store 1 zero bit
/// otherwise store 1 true bit and ref to cell
@pure
fun store_maybe_ref(b: builder, c: cell): builder
    asm(c b) "STOPTREF";


/***
  # Address manipulation primitives
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
fun load_msg_addr(s: slice): (slice, slice)
    asm( -> 1 0) "LDMSGADDR";

/// Decomposes slice [s] containing a valid `MsgAddress` into a `tuple t` with separate fields of this `MsgAddress`.
/// If [s] is not a valid `MsgAddress`, a cell deserialization exception is thrown.
@pure
fun parse_addr(s: slice): tuple
    asm "PARSEMSGADDR";

/// Parses slice [s] containing a valid `MsgAddressInt` (usually a `msg_addr_std`),
/// applies rewriting from the anycast (if present) to the same-length prefix of the address,
/// and returns both the workchain and the 256-bit address as integers.
/// If the address is not 256-bit, or if [s] is not a valid serialization of `MsgAddressInt`,
/// throws a cell deserialization exception.
@pure
fun parse_std_addr(s: slice): (int, int)
    asm "REWRITESTDADDR";

/// A variant of [parse_std_addr] that returns the (rewritten) address as a slice [s],
/// even if it is not exactly 256 bit long (represented by a `msg_addr_var`).
@pure
fun parse_var_addr(s: slice): (int, slice)
    asm "REWRITEVARADDR";

/***
  # Dictionary primitives
*/


/// Sets the value associated with [key_len]-bit key signed index in dictionary [dict] to [value] (cell),
/// and returns the resulting dictionary.
@pure
fun idict_set_ref(dict: cell, key_len: int, index: int, value: cell): cell
    asm(value index dict key_len) "DICTISETREF";

@pure
fun ~idict_set_ref(dict: cell, key_len: int, index: int, value: cell): (cell, ())
    asm(value index dict key_len) "DICTISETREF";

/// Sets the value associated with [key_len]-bit key unsigned index in dictionary [dict] to [value] (cell),
/// and returns the resulting dictionary.
@pure
fun udict_set_ref(dict: cell, key_len: int, index: int, value: cell): cell
    asm(value index dict key_len) "DICTUSETREF";

@pure
fun ~udict_set_ref(dict: cell, key_len: int, index: int, value: cell): (cell, ())
    asm(value index dict key_len) "DICTUSETREF";

@pure
fun idict_get_ref(dict: cell, key_len: int, index: int): cell
    asm(index dict key_len) "DICTIGETOPTREF";

@pure
fun idict_get_ref?(dict: cell, key_len: int, index: int): (cell, int)
    asm(index dict key_len) "DICTIGETREF" "NULLSWAPIFNOT";

@pure
fun udict_get_ref?(dict: cell, key_len: int, index: int): (cell, int)
    asm(index dict key_len) "DICTUGETREF" "NULLSWAPIFNOT";

@pure
fun idict_set_get_ref(dict: cell, key_len: int, index: int, value: cell): (cell, cell)
    asm(value index dict key_len) "DICTISETGETOPTREF";

@pure
fun udict_set_get_ref(dict: cell, key_len: int, index: int, value: cell): (cell, cell)
    asm(value index dict key_len) "DICTUSETGETOPTREF";

@pure
fun idict_delete?(dict: cell, key_len: int, index: int): (cell, int)
    asm(index dict key_len) "DICTIDEL";

@pure
fun udict_delete?(dict: cell, key_len: int, index: int): (cell, int)
    asm(index dict key_len) "DICTUDEL";

@pure
fun idict_get?(dict: cell, key_len: int, index: int): (slice, int)
    asm(index dict key_len) "DICTIGET" "NULLSWAPIFNOT";

@pure
fun udict_get?(dict: cell, key_len: int, index: int): (slice, int)
    asm(index dict key_len) "DICTUGET" "NULLSWAPIFNOT";

@pure
fun idict_delete_get?(dict: cell, key_len: int, index: int): (cell, slice, int)
    asm(index dict key_len) "DICTIDELGET" "NULLSWAPIFNOT";

@pure
fun udict_delete_get?(dict: cell, key_len: int, index: int): (cell, slice, int)
    asm(index dict key_len) "DICTUDELGET" "NULLSWAPIFNOT";

@pure
fun ~idict_delete_get?(dict: cell, key_len: int, index: int): (cell, (slice, int))
    asm(index dict key_len) "DICTIDELGET" "NULLSWAPIFNOT";

@pure
fun ~udict_delete_get?(dict: cell, key_len: int, index: int): (cell, (slice, int))
    asm(index dict key_len) "DICTUDELGET" "NULLSWAPIFNOT";

@pure
fun udict_set(dict: cell, key_len: int, index: int, value: slice): cell
    asm(value index dict key_len) "DICTUSET";

@pure
fun ~udict_set(dict: cell, key_len: int, index: int, value: slice): (cell, ())
    asm(value index dict key_len) "DICTUSET";

@pure
fun idict_set(dict: cell, key_len: int, index: int, value: slice): cell
    asm(value index dict key_len) "DICTISET";

@pure
fun ~idict_set(dict: cell, key_len: int, index: int, value: slice): (cell, ())
    asm(value index dict key_len) "DICTISET";

@pure
fun dict_set(dict: cell, key_len: int, index: slice, value: slice): cell
    asm(value index dict key_len) "DICTSET";

@pure
fun ~dict_set(dict: cell, key_len: int, index: slice, value: slice): (cell, ())
    asm(value index dict key_len) "DICTSET";

@pure
fun udict_add?(dict: cell, key_len: int, index: int, value: slice): (cell, int)
    asm(value index dict key_len) "DICTUADD";

@pure
fun udict_replace?(dict: cell, key_len: int, index: int, value: slice): (cell, int)
    asm(value index dict key_len) "DICTUREPLACE";

@pure
fun idict_add?(dict: cell, key_len: int, index: int, value: slice): (cell, int)
    asm(value index dict key_len) "DICTIADD";

@pure
fun idict_replace?(dict: cell, key_len: int, index: int, value: slice): (cell, int)
    asm(value index dict key_len) "DICTIREPLACE";

@pure
fun udict_set_builder(dict: cell, key_len: int, index: int, value: builder): cell
    asm(value index dict key_len) "DICTUSETB";

@pure
fun ~udict_set_builder(dict: cell, key_len: int, index: int, value: builder): (cell, ())
    asm(value index dict key_len) "DICTUSETB";

@pure
fun idict_set_builder(dict: cell, key_len: int, index: int, value: builder): cell
    asm(value index dict key_len) "DICTISETB";

@pure
fun ~idict_set_builder(dict: cell, key_len: int, index: int, value: builder): (cell, ())
    asm(value index dict key_len) "DICTISETB";

@pure
fun dict_set_builder(dict: cell, key_len: int, index: slice, value: builder): cell
    asm(value index dict key_len) "DICTSETB";

@pure
fun ~dict_set_builder(dict: cell, key_len: int, index: slice, value: builder): (cell, ())
    asm(value index dict key_len) "DICTSETB";

@pure
fun udict_add_builder?(dict: cell, key_len: int, index: int, value: builder): (cell, int)
    asm(value index dict key_len) "DICTUADDB";

@pure
fun udict_replace_builder?(dict: cell, key_len: int, index: int, value: builder): (cell, int)
    asm(value index dict key_len) "DICTUREPLACEB";

@pure
fun idict_add_builder?(dict: cell, key_len: int, index: int, value: builder): (cell, int)
    asm(value index dict key_len) "DICTIADDB";

@pure
fun idict_replace_builder?(dict: cell, key_len: int, index: int, value: builder): (cell, int)
    asm(value index dict key_len) "DICTIREPLACEB";

@pure
fun udict_delete_get_min(dict: cell, key_len: int): (cell, int, slice, int)
    asm(-> 0 2 1 3) "DICTUREMMIN" "NULLSWAPIFNOT2";

@pure
fun ~udict_delete_get_min(dict: cell, key_len: int): (cell, (int, slice, int))
    asm(-> 0 2 1 3) "DICTUREMMIN" "NULLSWAPIFNOT2";

@pure
fun idict_delete_get_min(dict: cell, key_len: int): (cell, int, slice, int)
    asm(-> 0 2 1 3) "DICTIREMMIN" "NULLSWAPIFNOT2";

@pure
fun ~idict_delete_get_min(dict: cell, key_len: int): (cell, (int, slice, int))
    asm(-> 0 2 1 3) "DICTIREMMIN" "NULLSWAPIFNOT2";

@pure
fun dict_delete_get_min(dict: cell, key_len: int): (cell, slice, slice, int)
    asm(-> 0 2 1 3) "DICTREMMIN" "NULLSWAPIFNOT2";

@pure
fun ~dict_delete_get_min(dict: cell, key_len: int): (cell, (slice, slice, int))
    asm(-> 0 2 1 3) "DICTREMMIN" "NULLSWAPIFNOT2";

@pure
fun udict_delete_get_max(dict: cell, key_len: int): (cell, int, slice, int)
    asm(-> 0 2 1 3) "DICTUREMMAX" "NULLSWAPIFNOT2";

@pure
fun ~udict_delete_get_max(dict: cell, key_len: int): (cell, (int, slice, int))
    asm(-> 0 2 1 3) "DICTUREMMAX" "NULLSWAPIFNOT2";

@pure
fun idict_delete_get_max(dict: cell, key_len: int): (cell, int, slice, int)
    asm(-> 0 2 1 3) "DICTIREMMAX" "NULLSWAPIFNOT2";

@pure
fun ~idict_delete_get_max(dict: cell, key_len: int): (cell, (int, slice, int))
    asm(-> 0 2 1 3) "DICTIREMMAX" "NULLSWAPIFNOT2";

@pure
fun dict_delete_get_max(dict: cell, key_len: int): (cell, slice, slice, int)
    asm(-> 0 2 1 3) "DICTREMMAX" "NULLSWAPIFNOT2";

@pure
fun ~dict_delete_get_max(dict: cell, key_len: int): (cell, (slice, slice, int))
    asm(-> 0 2 1 3) "DICTREMMAX" "NULLSWAPIFNOT2";

@pure
fun udict_get_min?(dict: cell, key_len: int): (int, slice, int)
    asm (-> 1 0 2) "DICTUMIN" "NULLSWAPIFNOT2";

@pure
fun udict_get_max?(dict: cell, key_len: int): (int, slice, int)
    asm (-> 1 0 2) "DICTUMAX" "NULLSWAPIFNOT2";

@pure
fun udict_get_min_ref?(dict: cell, key_len: int): (int, cell, int)
    asm (-> 1 0 2) "DICTUMINREF" "NULLSWAPIFNOT2";

@pure
fun udict_get_max_ref?(dict: cell, key_len: int): (int, cell, int)
    asm (-> 1 0 2) "DICTUMAXREF" "NULLSWAPIFNOT2";

@pure
fun idict_get_min?(dict: cell, key_len: int): (int, slice, int)
    asm (-> 1 0 2) "DICTIMIN" "NULLSWAPIFNOT2";

@pure
fun idict_get_max?(dict: cell, key_len: int): (int, slice, int)
    asm (-> 1 0 2) "DICTIMAX" "NULLSWAPIFNOT2";

@pure
fun idict_get_min_ref?(dict: cell, key_len: int): (int, cell, int)
    asm (-> 1 0 2) "DICTIMINREF" "NULLSWAPIFNOT2";

@pure
fun idict_get_max_ref?(dict: cell, key_len: int): (int, cell, int)
    asm (-> 1 0 2) "DICTIMAXREF" "NULLSWAPIFNOT2";

@pure
fun udict_get_next?(dict: cell, key_len: int, pivot: int): (int, slice, int)
    asm(pivot dict key_len -> 1 0 2) "DICTUGETNEXT" "NULLSWAPIFNOT2";

@pure
fun udict_get_nexteq?(dict: cell, key_len: int, pivot: int): (int, slice, int)
    asm(pivot dict key_len -> 1 0 2) "DICTUGETNEXTEQ" "NULLSWAPIFNOT2";

@pure
fun udict_get_prev?(dict: cell, key_len: int, pivot: int): (int, slice, int)
    asm(pivot dict key_len -> 1 0 2) "DICTUGETPREV" "NULLSWAPIFNOT2";

@pure
fun udict_get_preveq?(dict: cell, key_len: int, pivot: int): (int, slice, int)
    asm(pivot dict key_len -> 1 0 2) "DICTUGETPREVEQ" "NULLSWAPIFNOT2";

@pure
fun idict_get_next?(dict: cell, key_len: int, pivot: int): (int, slice, int)
    asm(pivot dict key_len -> 1 0 2) "DICTIGETNEXT" "NULLSWAPIFNOT2";

@pure
fun idict_get_nexteq?(dict: cell, key_len: int, pivot: int): (int, slice, int)
    asm(pivot dict key_len -> 1 0 2) "DICTIGETNEXTEQ" "NULLSWAPIFNOT2";

@pure
fun idict_get_prev?(dict: cell, key_len: int, pivot: int): (int, slice, int)
    asm(pivot dict key_len -> 1 0 2) "DICTIGETPREV" "NULLSWAPIFNOT2";

@pure
fun idict_get_preveq?(dict: cell, key_len: int, pivot: int): (int, slice, int)
    asm(pivot dict key_len -> 1 0 2) "DICTIGETPREVEQ" "NULLSWAPIFNOT2";

/// Creates an empty dictionary, which is actually a null value. Equivalent to PUSHNULL
@pure
fun new_dict(): cell
    asm "NEWDICT";

/// Checks whether a dictionary is empty.
@pure
fun dict_empty?(c: cell): int
    asm "DICTEMPTY";


/* Prefix dictionary primitives */
@pure
fun pfxdict_get?(dict: cell, key_len: int, key: slice): (slice, slice, slice, int)
    asm(key dict key_len) "PFXDICTGETQ" "NULLSWAPIFNOT2";

@pure
fun pfxdict_set?(dict: cell, key_len: int, key: slice, value: slice): (cell, int)
    asm(value key dict key_len) "PFXDICTSET";

@pure
fun pfxdict_delete?(dict: cell, key_len: int, key: slice): (cell, int)
    asm(key dict key_len) "PFXDICTDEL";

/// Returns the value of the global configuration parameter with integer index `i` as a `cell` or `null` value.
@pure
fun config_param(x: int): cell
    asm "CONFIGOPTPARAM";

/// Creates an output action which would reserve exactly amount nanotoncoins (if mode = 0), at most amount nanotoncoins (if mode = 2), or all but amount nanotoncoins (if mode = 1 or mode = 3), from the remaining balance of the account. It is roughly equivalent to creating an outbound message carrying amount nanotoncoins (or b − amount nanotoncoins, where b is the remaining balance) to oneself, so that the subsequent output actions would not be able to spend more money than the remainder. Bit +2 in mode means that the external action does not fail if the specified amount cannot be reserved; instead, all remaining balance is reserved. Bit +8 in mode means `amount <- -amount` before performing any further actions. Bit +4 in mode means that amount is increased by the original balance of the current account (before the compute phase), including all extra currencies, before performing any other checks and actions. Currently, amount must be a non-negative integer, and mode must be in the range 0..15.
fun raw_reserve(amount: int, mode: int): void
    asm "RAWRESERVE";

/// Similar to raw_reserve, but also accepts a dictionary extra_amount (represented by a cell or null) with extra currencies. In this way currencies other than TonCoin can be reserved.
fun raw_reserve_extra(amount: int, extra_amount: cell, mode: int): void
    asm "RAWRESERVEX";

/// Sends a raw message contained in msg, which should contain a correctly serialized object Message X, with the only exception that the source address is allowed to have dummy value addr_none (to be automatically replaced with the current smart contract address), and ihr_fee, fwd_fee, created_lt and created_at fields can have arbitrary values (to be rewritten with correct values during the action phase of the current transaction). Integer parameter mode contains the flags. Currently mode = 0 is used for ordinary messages; mode = 128 is used for messages that are to carry all the remaining balance of the current smart contract (instead of the value originally indicated in the message); mode = 64 is used for messages that carry all the remaining value of the inbound message in addition to the value initially indicated in the new message (if bit 0 is not set, the gas fees are deducted from this amount); mode' = mode + 1 means that the sender wants to pay transfer fees separately; mode' = mode + 2 means that any errors arising while processing this message during the action phase should be ignored. Finally, mode' = mode + 32 means that the current account must be destroyed if its resulting balance is zero. This flag is usually employed together with +128.
fun send_raw_message(msg: cell, mode: int): void
    asm "SENDRAWMSG";

/// Creates an output action that would change this smart contract code to that given by cell new_code. Notice that this change will take effect only after the successful termination of the current run of the smart contract
fun set_code(new_code: cell): void
    asm "SETCODE";

/// Generates a new pseudo-random unsigned 256-bit integer x. The algorithm is as follows: if r is the old value of the random seed, considered as a 32-byte array (by constructing the big-endian representation of an unsigned 256-bit integer), then its sha512(r) is computed; the first 32 bytes of this hash are stored as the new value r' of the random seed, and the remaining 32 bytes are returned as the next random value x.
fun random(): int
    asm "RANDU256";

/// Generates a new pseudo-random integer z in the range 0..range−1 (or range..−1, if range < 0). More precisely, an unsigned random value x is generated as in random; then z := x * range / 2^256 is computed.
fun rand(range: int): int
    asm "RAND";

/// Returns the current random seed as an unsigned 256-bit Integer.
@pure
fun get_seed(): int
    asm "RANDSEED";

/// Sets the random seed to unsigned 256-bit seed.
fun set_seed(seed: int): void
    asm "SETRAND";

/// Mixes unsigned 256-bit integer x into the random seed r by setting the random seed to sha256 of the concatenation of two 32-byte strings: the first with the big-endian representation of the old seed r, and the second with the big-endian representation of x.
fun randomize(x: int): void
    asm "ADDRAND";

/// Equivalent to randomize(cur_lt());.
fun randomize_lt(): void
    asm "LTIME" "ADDRAND";

/// Checks whether the data parts of two slices coinside
@pure
fun equal_slice_bits(a: slice, b: slice): int
    asm "SDEQ";

/// Concatenates two builders
@pure
fun store_builder(to: builder, from: builder): builder
    asm "STBR";
