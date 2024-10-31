// Here we test "functions that just wrap other functions" (camelCase in particular):
// > builder beginCell() { return begin_cell(); }
// Such functions, when called, are explicitly inlined during code generation (even without `inline` modifier).
// It means, that `beginCell()` is replaced to `begin_cell()` (and effectively to `NEWC`).
// Moreover, body of `beginCell` is NOT codegenerated at all.
// Hence, we can write camelCase wrappers (as well as more intelligible namings around stdlib functions)
// without affecting performance and even bytecode hashes.
// This works with ~functions also. And even works with wrappers of wrappers.
// Moreover, such wrappers can reorder input parameters, see a separate test camel2.tolk.

fun myBeginCell(): builder { return begin_cell(); }
fun myEndCell(b: builder): cell { return end_cell(b); }
fun myStoreRef(b: builder, c: cell): builder { return store_ref(b, c); }
fun myStoreUint(b: builder, i: int, bw: int): builder { return store_uint(b, i, bw); }

// 'inline' is not needed actually, but if it exists, it's just ignored
@inline
@pure
fun myBeginParse(c: cell): slice { return begin_parse(c); }
@inline
@pure
fun mySkipBits(s: slice, len: int): slice { return skip_bits(s, len); }
@inline
@pure
fun ~mySkipBits(s: slice, len: int): (slice, ()) { return ~skip_bits(s, len); }
@inline
@pure
fun ~myLoadUint(s: slice, len: int): (slice, int) { return load_uint(s, len); }

fun myComputeDataSize(c: cell, maxCells: int): (int, int, int) { return compute_data_size(c, maxCells); }

fun dict__new(): cell { return new_dict(); }
fun dict__iset(dict: cell, keyLen: int, index: int, value: slice): cell { return idict_set(dict, keyLen, index, value); }
fun ~dict__iset(dict: cell, keyLen: int, index: int, value: slice): (cell, ()) { return ~idict_set(dict, keyLen, index, value); }
fun dict__tryIGet(dict: cell, keyLen: int, index: int): (slice, int) { return idict_get?(dict, keyLen, index); }
fun dict__tryIGetMin(dict: cell, keyLen: int): (int, slice, int) { return idict_get_min?(dict, keyLen); }

fun myEmptyTuple(): tuple { return empty_tuple(); }
fun emptyTuple1(): tuple { return myEmptyTuple(); }
fun emptyTuple11(): tuple { return emptyTuple1(); }
fun myTuplePush<X>(t: tuple, value: X): tuple { return tpush(t, value); }
fun ~myTuplePush<X>(t: tuple, value: X): (tuple, ()) { return ~tpush(t, value); }
fun myTupleAt<X>(t: tuple, index: int): X { return at(t, index); }
fun tripleSecond<X1, Y2, Z3>(p: [X1, Y2, Z3]): Y2 { return triple_second(p); }
@pure
fun nullValue<X>(): X
asm "PUSHNULL";

fun initial1(x: tuple): tuple { return x; }
fun initial2(x: tuple): tuple { return initial1(x); }

// int add(int x, int y) { return x + y; } // this is also a wrapper, as its body is _+_(x,y)

fun fake1(a: int, b: int, c: int): void
asm(a b c) "DROP DROP DROP";
fun fake2(a: int, b: int, c: int): void
asm(b c a) "DROP DROP DROP";
fun fake3(a: int, b: int, c: int): ()
asm(c a b) "DROP DROP DROP";
fun fake4(a: int, b: int, c: int): ()
asm(c b a) "DROP DROP DROP";

fun fake1Wrapper(a: int, b: int, c: int) { return fake1(a, b, c); }
fun fake2Wrapper(a: int, b: int, c: int) { return fake2(a, b, c); }
fun fake3Wrapper(a: int, b: int, c: int) { return fake3(a, b, c); }
fun fake4Wrapper(a: int, b: int, c: int) { return fake4(a, b, c); }

@method_id(101)
fun test1(): [int, int, int] {
    var x: int = 1;
    var y: int = 1;
    var to_be_ref: cell = myBeginCell().myEndCell();
    var in_c: builder = myBeginCell().myStoreUint(123, 8);
    in_c = myStoreRef(in_c, to_be_ref);
    var (a, b, c) = myComputeDataSize(in_c.myEndCell(), 10);
    assert(!(b != 8)) throw 101;
    assert(!(c != 1), 101);
    return [a, b + x, c + y];
}

@method_id(102)
fun test2(): [[int, int, int], int, int, int] {
    var dict: cell = dict__new();
    dict = dict__iset(dict, 32, 456, myBeginCell().myStoreUint(4560, 32).myEndCell().myBeginParse());
    dict.dict__iset(32, 789, myBeginCell().myStoreUint(7890, 32).myEndCell().myBeginParse());
    dict~dict__iset(32, 123, myBeginCell().myStoreUint(0, 64).myStoreUint(1230, 32).myStoreUint(1231, 32).myStoreUint(1232, 32).myEndCell().myBeginParse());

    var (mink, minv, _) = dict__tryIGetMin(dict, 32);
    // skip 64 bits
    minv~mySkipBits(16);
    minv = minv.mySkipBits(16);
    minv.mySkipBits(11);             // does nothing
    (minv, _) = ~mySkipBits(minv, 16);
    mySkipBits(minv, 11);         // does nothing
    minv~mySkipBits(16);
    // load 3*32
    var minv1 = minv~myLoadUint(32);
    var minv2 = minv~myLoadUint(32);
    var minv3 = minv~myLoadUint(32);

    var (_, found123) = dict__tryIGet(dict, 32, 123);
    var (_, found456) = dict__tryIGet(dict, 32, 456);
    var (_, found789) = dict__tryIGet(dict, 32, 789);
    return [[minv1, minv2, minv3], found123, found456, found789];
}

@method_id(103)
fun test3(): tuple {
    var with34: tuple = initial2(emptyTuple1());
    with34~myTuplePush(34);

    var t: tuple = emptyTuple11();
    t = myTuplePush(t, 12);
    myTuplePush(t, emptyTuple11());    // does nothing
    t~myTuplePush(emptyTuple1());
    t~myTuplePush(with34.myTupleAt(0));
    t.myTuplePush("123"s);   // does nothing

    var tri: [cell, int, cell] = [nullValue(), 90 + 1, null];
    var f: int = tripleSecond(tri);
    (t, _) = ~myTuplePush(t, f);

    return t;
}

@method_id(104)
fun test4(a: int, b: int, c: int): int {
    fake1Wrapper(a, b, c);
    fake2Wrapper(a, b, c);
    fake3Wrapper(a, b, c);
    fake4Wrapper(a, b, c);
    return 10;
}

fun main(): int {
    var x: int = now();
    return 30;
}

/**
    method_id  | in | out
@testcase | 101 |    | [ 2 9 2 ]
@testcase | 102 |    | [ [ 1230 1231 1232 ] -1 -1 0 ]
@testcase | 103 |    | [ 12 [] 34 91 ]

@fif_codegen
"""
  main PROC:<{
    //
    30 PUSHINT
  }>
"""

@fif_codegen
"""
  test1 PROC:<{
    //
    NEWC        //  _5
    ENDC        //  to_be_ref
    NEWC        //  to_be_ref _8
    123 PUSHINT //  to_be_ref _8 _9=123
    SWAP        //  to_be_ref _9=123 _8
    8 STU       //  to_be_ref in_c
    STREF       //  in_c
    ENDC        //  _16
    10 PUSHINT  //  _16 _17=10
    CDATASIZE   //  a b c
    OVER        //  a b c b
    8 NEQINT    //  a b c _21
    101 THROWIF
    DUP //  a b c c
    1 NEQINT    //  a b c _26
    101 THROWIF
    SWAP        //  a c b
    INC //  a c _30
    SWAP        //  a _30 c
    INC //  a _30 _31
    TRIPLE      //  _29
  }>
"""

@fif_codegen
"""
  test2 PROC:<{
    ...
    16 PUSHINT	//  dict minv _45=16
    SDSKIPFIRST	//  dict minv
    16 PUSHINT	//  dict minv _47=16
    SDSKIPFIRST	//  dict minv
    16 PUSHINT	//  dict minv _52=16
    SDSKIPFIRST	//  dict minv
    16 PUSHINT	//  dict minv _57=16
    SDSKIPFIRST	//  dict minv
    ...
    32 PUSHINT  //  dict minv1 minv2 minv3 found123 found456 _83=32
    789 PUSHINT //  dict minv1 minv2 minv3 found123 found456 _83=32 _84=789
    s0 s7 s7 XCHG3      //  found456 minv1 minv2 minv3 found123 _84=789 dict _83=32
    DICTIGET
    NULLSWAPIFNOT       //  found456 minv1 minv2 minv3 found123 _101 _102
    NIP //  found456 minv1 minv2 minv3 found123 found789
    ...
    4 TUPLE	//  _86
  }>
"""

@fif_codegen
"""
  test3 PROC:<{
    //
    NIL	//  _1
    initial1 CALLDICT // with34
    ...
    TRIPLE	//  t tri
    SECOND	//  t f
    TPUSH	//  t
  }>
"""

@fif_codegen
"""
  test4 PROC:<{
    //  a b c
    s2 s1 s0 PUSH3  //  a b c a b c
    DROP DROP DROP
    s1 s0 s2 PUSH3  //  a b c b c a
    DROP DROP DROP
    s0 s2 s1 PUSH3  //  a b c c a b
    DROP DROP DROP
    s0 s2 XCHG  //  c b a
    DROP DROP DROP
    10 PUSHINT  //  _7=10
  }>
"""

@fif_codegen_avoid DECLPROC myBeginCell
@fif_codegen_avoid DECLPROC myStoreUint
@fif_codegen_avoid DECLPROC myStoreRef
@fif_codegen_avoid DECLPROC myComputeDataSize
@fif_codegen_avoid DECLPROC tryIdictGet
@fif_codegen_avoid DECLPROC myEmptyTuple
@fif_codegen_avoid DECLPROC myStoreUint
@fif_codegen_avoid DECLPROC initial2
@fif_codegen_avoid DECLPROC add
@fif_codegen_avoid DECLPROC increase
*/
