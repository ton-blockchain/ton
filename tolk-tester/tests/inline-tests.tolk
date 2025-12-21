fun foo1(x: int): int {
    __expect_inline(false);     // returns in the middle, leave it for Fift
    if (x == 1) {
        return 1;
    }
    return 2;
}

@inline
fun foo2(x: int): int {
    __expect_inline(false);     // returns in the middle, leave it for Fift
    if (x == 1) {
        return 11;
    }
    return 22;
}

@inline_ref
fun foo3(x: int): int {
    if (x == 1) {
        return 111;
    }
    return 222;
}

@inline_ref
fun foo_light_but_ref() {
    __expect_inline(false);
    return 0;
}

@noinline
fun foo_light_but_no() {
    __expect_inline(false);
    return 0;
}

@method_id(101)
fun test1(x: int): (int, int, int) {
    __expect_inline(false);     // @method_id
    return (foo1(x) + 1, foo2(x) + 1, foo3(x) + 1 + foo_light_but_ref() + foo_light_but_no());
}

global g: int;

@inline
fun foo_repeat() {
    __expect_inline(true);
    g = 1;
    repeat (5) {
        g *= 2;
    }
}

@inline
fun foo_until(): int {
    __expect_inline(true);
    g = 1;
    var i: int = 0;
    do {
        g *= 2;
        i += 1;
    } while (i < 8);
    return i;
}

@inline
fun foo_while(): int {
    __expect_inline(true);
    g = 1;
    var i: int = 0;
    while (i < 10) {
        g *= 2;
        i += 1;
    }
    return i;
}

@method_id(102)
fun test2() {
    foo_repeat();
    var x: int = g;
    foo_until();
    var y: int = g;
    foo_while();
    var z: int = g;
    return (x, y, z);
}

@inline
fun foo_big(x: int): int {
    __expect_inline(true);
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    return x;
}

@noinline
fun foo_big_not_annotated_called_once(x: int): int {
    __expect_inline(true);     // called only once, inline
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    return x;
}

fun foo_big_not_annotated_called_twice(x: int): int {
    __expect_inline(false);     // too big for in-place auto-inlining when called multiple times
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1; x = x * 10 + 1;
    return x;
}

@method_id(103)
fun test3(x: int): int {
    return foo_big(x) * 10 + 5;
}

@method_id(104)
fun test4(x: int) {
    return (foo_big_not_annotated_called_once(x) * 10, foo_big_not_annotated_called_twice(x) * 10, foo_big_not_annotated_called_twice(x));
}

struct Storage {
    owner: address;
    lastCall: int64;
    extra: Cell<(int8, uint256)>;
}

fun Storage.load() {
    __expect_inline(true);
    return Storage.fromCell(contract.getData());
}

fun Storage.save(self) {
    __expect_inline(true);
    contract.setData(self.toCell());
}

@method_id(105)
fun test5() {
    var st = Storage.load();
    st.lastCall = 0;
    st.save();
}

@inline
fun factorial(x: int): int {
    __expect_inline(false);   // due to recursion
    if (x <= 1) {
        return 1;
    }
    return x * factorial(x - 1);
}

fun recurse1(x: int): int { __expect_inline(false); return recurse2(x + 1); }
fun recurse2(x: int): int { __expect_inline(false); return recurse3(x + 1); }
fun recurse3(x: int): int { __expect_inline(false); return recurse1(x + 1); }

global g10: int;

fun startCellWith(x: int) {
    __expect_inline(true);
    return beginCell().storeUint(x, 32);
}

fun startCellWithG10() {
    __expect_inline(true);
    return beginCell().storeUint(g10, 32);
}

fun builder.myBitsCount(self): int {
    __expect_inline(true);
    return self.bitsCount();
}

fun builder.store32(mutate self, v: int32): self {
    __expect_inline(true);
    return self.storeUint(v, 32);
}

fun sum(a: int, b: int) {
    __expect_inline(true);
    return a + b;
}

fun eq<T>(v: T): T {
    __expect_inline(true);
    return v;
}

fun get20() { return 20; }

struct Point {
    x: int;
    y: int;
}

fun Point.getX(self) {
    __expect_inline(true);
    return self.x;
}

fun Point.create(x: int, y: int): Point {
    __expect_inline(true);
    return {x, y}
}

fun increment(mutate x: int) {
    __expect_inline(true);
    x += 1;
}

@method_id(106)
fun test6() {
    var p = Point.create(10, 20);
    increment(mutate p.x);
    p.y = p.getX();
    return p;
}

@method_id(107)
fun test7() {
    g10 = 10;
    return startCellWithG10().store32(20).myBitsCount();
}

@method_id(108)
fun test8() {
    var x = 0;
    return (startCellWith(x = 5).store32(x += 20).store32(x), x);
}

@method_id(109)
fun test9(x: int) {
    var p1: Point = {x, y: 0};
    var p2: Point = {x: 0, y: p1.getX()};
    var t = createEmptyTuple();
    t.pushPoint(p1);
    t.pushPoint(p2);
    return (t.popPoint(), t.popPoint(), t);
}

fun tuple.pushPoint(mutate self, p: Point) {
    __expect_inline(true);
    var x = p.x;
    self.push(x);
    self.push(p.y);
}

fun tuple.popPoint(mutate self): Point {
    __expect_inline(true);
    var y = self.pop<int>();
    return {x: self.pop(), y};
}

@method_id(110)
fun test10() {
    var (a, b, c) = usedIn10ButDeclaredBelow(5);
    return (a, b, c.bitsCount());
}

global g11_1: int;
global g11_2: int;

fun mutateGlobals(mutate v: int, inc11_2: int) {
    __expect_inline(true);
    v += 1;
    g11_2 += inc11_2;
}

@method_id(111)
fun test11() {
    g11_1 = 0;
    g11_2 = 0;
    mutateGlobals(mutate g11_1, 5);         // 1 5
    mutateGlobals(mutate g11_1, g11_2);     // 2 10
    mutateGlobals(mutate g11_2, g11_1);     // 2 11
    return (g11_1, g11_2);
}

fun nested1(x: int) {
    var y = x + 2;
    x += 7;
    return y;
}

fun nested2(x: int) {
    var y = x + 2;
    var z = nested1(y);
    y += z;
    return y + z;
}

@method_id(112)
fun test12(x: int) {
    var r1 = nested2(10);
    var r2 = nested2(x);
    return (r1, r2);
}

fun anotherMath(arg: int) {
    __expect_inline(true);
    var cp_x = arg;
    increment(mutate arg);
    return cp_x + arg;
}

@method_id(113)
fun test13(p: Point) {
    anotherMath(p.x);
    var r = anotherMath(p.y);
    return (p, r);
}

fun evalPoint(p: Point) {
    __expect_inline(true);
    increment(mutate p.x);
    p.y *= 2;
    return max(p.x, p.y);
}

@method_id(114)
fun test14(p2: Point) {
    var p1: Point = { x: 0, y: 0 };
    var r1 = evalPoint(p1);
    return (r1, p1, evalPoint(p2), p2);
}

global t: tuple;

fun logT(a: int) {
    __expect_inline(true);
    t.push(a);
}

fun demoInlined1(x: int) {
    if (x == -1) {
        logT(10);
    } else if (x == 0) {
        logT(20);
    }
}

fun demoInlined2(x: int) {
    __expect_inline(true);
    match (x) {
        -1 => logT(10),
        else => logT(20),
    }
}

fun wrapperNotInlined(x: int?) {
    if (x != null) {
        demoInlined1(x);
        return;
    }
    logT(100);
}

fun someUsages(x: int?) {
    __expect_inline(true);
    wrapperNotInlined(x);
    demoInlined2(x!);
    return;
}

@method_id(115)
fun test15(x: int?) {
    t = createEmptyTuple();
    someUsages(x);
    return t;
}

struct (0x01) A16 { a: int8; }
struct (0x02) B16 {}
struct (0x03) C16 {}
type U16 = A16 | B16 | C16;

fun check16(msg: U16) {
    match (msg) {
        A16 => { assert(msg.a > 0, 100); }
        B16 => {}
        C16 => {}
    }
}

fun wrap16(msg: U16, check: bool) {
    if (check) {
        check16(msg);
        return;
    }
    throw 123;
}

@method_id(116)
fun test16() {
    wrap16(U16.fromSlice(stringHexToSlice("0170")), true);
    return -1;
}


fun main() {
    // regardless of number of calls, it will be inlined, since it's lightweight in AST terms
    Storage.load(); Storage.load(); Storage.load(); Storage.load(); Storage.load();
    Storage.load(); Storage.load(); Storage.load(); Storage.load(); Storage.load();
    Storage.load(); Storage.load(); Storage.load(); Storage.load(); Storage.load();
    Storage.load(); Storage.load(); Storage.load(); Storage.load(); Storage.load();
    Storage.load(); Storage.load(); Storage.load(); Storage.load(); Storage.load();
    usedIn10ButDeclaredBelow(10);
}

fun usedIn10ButDeclaredBelow(x: int) {
    __expect_inline(true);
    var y = eq(x);
    return (x, y, eq(startCellWith(x)));
}



/**
      method_id | in     | out
@testcase | 101 | 1      | 2 12 112
@testcase | 101 | 2      | 3 23 223
@testcase | 102 |        | 32 256 1024
@testcase | 103 | 9      | 911111111111111111111111111111111111111111111111115
@testcase | 104 | 9      | 911111111111111111111111111110 911111111111111111111111111110 91111111111111111111111111111
@testcase | 106 |        | 11 11
@testcase | 107 |        | 64
@testcase | 108 |        | BC{0018000000050000001900000019} 25
@testcase | 109 | 10     | 0 10 10 0 []
@testcase | 110 |        | 5 5 32
@testcase | 111 |        | 2 11
@testcase | 112 | 10     | 40 40
@testcase | 113 | 10 20  | 10 20 41
@testcase | 114 | 10 20  | 1 0 0 40 10 20
@testcase | 115 | -1     | [ 10 10 ]
@testcase | 115 | 1      | [ 20 ]
@testcase | 116 |        | -1

@fif_codegen_avoid Storage.load
@fif_codegen_avoid Storage.save
@fif_codegen
"""
  test5() PROC:<{               //
    c4 PUSH                     //  '7
    CTOS                        //  s
    LDSTDADDR                   //  '11 s
"""

@fif_codegen
"""
  test6() PROC:<{
    11 PUSHINT
    DUP
  }>
"""

@fif_codegen
"""
  test8() PROC:<{               //
    NEWC
    x{00000005} STSLICECONST    //  '3
    25 PUSHINT                  //  '3 '9
    SWAP                        //  x self
    x{0000001900000019} STSLICECONST  //  x '3
    SWAP                        //  '3 x
  }>
"""

@fif_codegen
"""
  test9() PROC:<{               //  x
    0 PUSHINT                   //  p1.x p1.y=0
    s0 s1 PUSH2                 //  p1.x p1.y=0 p2.x=0 p2.y
    NIL                         //  x p1.y=0 p2.x=0 p2.y self
    s0 s4 XCHG2                 //  p2.y p1.y=0 p2.x=0 self x
    TPUSH                       //  p2.y p1.y=0 p2.x=0 self
    ROT                         //  p2.y p2.x=0 self p1.y=0
    TPUSH                       //  p2.y x=0 self
    SWAP                        //  p2.y self x=0
    TPUSH                       //  p2.y self
    SWAP                        //  self p2.y
    TPUSH                       //  self
    TPOP                        //  self y
    SWAP                        //  y self
    TPOP                        //  '36 self '35
    SWAP                        //  '36 '35 self
    TPOP                        //  '36 '35 self y
    SWAP                        //  '36 '35 y self
    TPOP                        //  '36 '35 '50 t '49
    s3 s4 XCHG                  //  '35 '36 '50 t '49
    -ROT                        //  '35 '36 '49 '50 t
  }>
"""

@fif_codegen
"""
  test10() PROC:<{
    5 PUSHINT                   //  '3=5
    DUP                         //  '3=5 y=5
    NEWC
    x{00000005} STSLICECONST    //  a=5 b=5 c
    BBITS                       //  a=5 b=5 '18
  }>
"""

@fif_codegen
"""
  test12() PROC:<{              //  x
    40 PUSHINT                  //  x r1
    SWAP                        //  r1 x
    2 ADDCONST                  //  r1 y
    ...
"""

@fif_codegen
"""
  wrapperNotInlined() PROC:<{   //  x
    DUP                         //  x x
    ISNULL                      //  x '1
    IFNOTJMP:<{                 //  x
      DUP                       //  x x
      -1 EQINT                  //  x '3
      IFJMP:<{                  //  x
"""

@fif_codegen
"""
  wrap16() PROC:<{
    IFJMP:<{
      129 PUSHINT
      OVER
      EQUAL
      IFJMP:<{
        DROP
        0 GTINT
        100 THROWIFNOT
      }>
      130 PUSHINT
      s2 POP
      EQUAL
      IFJMP:<{
      }>
    }>
    123 THROW
  }>
"""
*/
