// Here we also test "functions that just wrap other functions" like in camel1.tolk,
// but when they reorder arguments, e.g.
// > T f(x,y) { return anotherF(y,x); }
// This also works, even for wrappers of wrappers, even if anotherF is asm(with reorder).
// But swapping arguments may sometimes lead to bytecode changes (see test2),
// both with compute-asm-ltr and without it.

fun myBeginCell(): builder { return begin_cell(); }
fun myEndCell(b: builder): cell { return end_cell(b); }
fun myStoreRef1(b: builder, c: cell): builder { return store_ref(b, c); }
fun myStoreRef2(c: cell, b: builder): builder { return store_ref(b, c); }
fun myStoreUint1(b: builder, x: int, bw: int): builder { return store_uint(b, x, bw); }
fun myStoreUint2(b: builder, bw: int, x: int): builder { return store_uint(b, x, bw); }

fun computeDataSize1(c: cell, maxCells: int): (int, int, int) { return compute_data_size(c, maxCells); }
fun computeDataSize2(maxCells: int, c: cell): (int, int, int) { return compute_data_size(c, maxCells); }

fun fake(a: int, b: int, c: int): void
asm "DROP DROP DROP";
fun fake2(b: int, c: int, a: int) { return fake(a,b,c); }
fun fake3(c: int, a: int, b: int) { return fake(a,b,c); }
fun fake4(c: int, b: int, a: int) { return fake(a,b,c); }

@method_id(101)
fun test1(): (int, int, int) {
    var x: int = 1;
    var y: int = 1;
    var to_be_ref: cell = myBeginCell().myEndCell();
    var in_c: builder = myBeginCell().myStoreUint1(123, 8);
    in_c = myStoreRef1(in_c, to_be_ref);
    var (a, b, c) = computeDataSize1(in_c.myEndCell(), 10);
    assert(!0, 101);
    return (a, b + x, c + y);
}

@method_id(102)
fun test2(): (int, int, int) {
    var x: int = 1;
    var y: int = 1;
    var to_be_ref: cell = myBeginCell().myEndCell();
    var in_c: builder = myBeginCell().myStoreUint2(8, 123);
    in_c = myStoreRef2(to_be_ref, in_c);
    var (a, b, c) = computeDataSize2(10, in_c.myEndCell());
    return (a, b + x, c + y);
}

@method_id(103)
fun test3(): (int, int, int) {
    var x: int = 1;
    var y: int = 1;
    var to_be_ref: cell = begin_cell().end_cell();
    var in_c: builder = begin_cell().store_uint(123, 8);
    in_c = store_ref(in_c, to_be_ref);
    var (a, b, c) = compute_data_size(in_c.end_cell(), 10);
    return (a, b + x, c + y);
}

fun beginCell1(): builder { return begin_cell(); }
fun beginCell11(): builder { return beginCell1(); }
fun beginCell111(): builder { return beginCell11(); }

fun endCell1(b: builder): cell { return end_cell(b); }
fun endCell11(b: builder): cell { return endCell1(b); }

fun beginParse1(c: cell): slice { return begin_parse(c); }
fun beginParse11(c: cell): slice { return beginParse1(c); }

fun storeInt1(b: builder, bw: int, x: int): builder { return store_int(b, x, bw); }
fun storeInt11(bw: int, x: int, b: builder): builder { return storeInt1(b, bw, x); }
fun storeInt111(b: builder, x: int, bw: int): builder { return storeInt11(bw, x, b); }

@method_id(104)
fun test4(): slice {
    var b: builder = beginCell111();
    b = storeInt11(32, 1, b);
    b = storeInt111(b, 2, 32).storeInt111(3, 32);
    return b.endCell11().beginParse11();
}

@method_id(105)
fun test5(a: int, b: int, c: int): int {
    fake(a, b, c);
    fake2(b, c, a);
    fake3(c, a, b);
    fake4(c, b, a);
    return a;
}

fun main() {
    throw 0;
}

/**
    method_id  | in | out
@testcase | 101 |    | 2 9 2
@testcase | 102 |    | 2 9 2
@testcase | 103 |    | 2 9 2
@testcase | 104 |    | CS{Cell{0018000000010000000200000003} bits: 0..96; refs: 0..0}

test1 and test3 fif code is absolutely identical, test2 (due to reorder) is a bit different:

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
    SWAP        //  a c b
    INC //  a c _23
    SWAP        //  a _23 c
    INC //  a _23 _24
  }>
"""

@fif_codegen
"""
  test2 PROC:<{
    //
    NEWC        //  _5
    ENDC        //  to_be_ref
    NEWC        //  to_be_ref _8
    123 PUSHINT //  to_be_ref _8 _10=123
    SWAP        //  to_be_ref _10=123 _8
    8 STU       //  to_be_ref in_c
    STREF       //  in_c
    10 PUSHINT
    SWAP
    ENDC
    SWAP
    CDATASIZE   //  a b c
    SWAP        //  a c b
    INC         //  a c _19
    SWAP        //  a _19 c
    INC         //  a _19 _20
  }>
"""

@fif_codegen
"""
  test3 PROC:<{
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
    SWAP        //  a c b
    INC //  a c _19
    SWAP        //  a _19 c
    INC //  a _19 _20
  }>
"""

@fif_codegen
"""
  test4 PROC:<{
    //
    NEWC	//  b
    1 PUSHINT	//  b _3=1
    SWAP	//  _3=1 b
    32 STI	//  b
    2 PUSHINT
    SWAP       // _5=2 b
    32 STI
    3 PUSHINT
    SWAP
    32 STI	//  b
    ENDC	//  _11
    CTOS	//  _12
  }>
"""

@fif_codegen
"""
  test5 PROC:<{
    //  a b c
    s2 s1 s0 PUSH3  //  a b c a b c
    DROP DROP DROP
    s2 s1 s0 PUSH3  //  a b c a b c
    DROP DROP DROP
    s2 s1 s0 PUSH3  //  a b c a b c
    DROP DROP DROP
    s2 PUSH
    -ROT            // a a b c
    DROP DROP DROP
  }>
"""

@fif_codegen_avoid myStoreUint1
@fif_codegen_avoid myStoreUint2
*/
