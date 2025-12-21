const TWO = 2;
global gTup: tuple;

fun get1() { gTup.push(1); return 1; }
fun get2() { gTup.push(2); return 2; }

struct Point {
    x: int;
    y: int;
}

@noinline
fun Point.create0NoInline(): Point {
    return { x: 0, y: 0 }
}

fun Point.create0(): Point {
    return { x: 0, y: 0 }
}

@method_id(101)
fun test101(x: int) {
    return (x ? 2 : 1, x == 5 ? TWO : 1);
}

@method_id(102)
fun test102(x: int?) {
    return x == null ? (0!) : ((x));
}

@method_id(103)
fun test103(x: bool) {
    if (x ? -1 : +1) {
        return x ? -2 : +2;
    }
    throw 123;
}

@method_id(104)
fun test104(x: (int, int)) {
    return x.0 ? true : null;
}

@method_id(105)
fun test105(getX: bool) {
    var p: Point = { x: 10, y: 20 };
    return getX ? p.x : p!.y;
}

@noinline
fun helper106(glob: bool, t: tuple) {
    return glob ? gTup : t;
}

@method_id(106)
fun test106() {
    gTup = createEmptyTuple();
    gTup.push(1);
    return helper106(true, createEmptyTuple());
}

@noinline
fun helper107(t: (int, Point)) {
    return t.0 ? t.0 : t.1.y;
}

@method_id(107)
fun test107() {
    return (helper107((1, {x: 10, y:20})), helper107((0, {x: 10, y: 20})));
}

@method_id(108)
fun test108(calcMin: bool) {
    var cb = calcMin ? min : max;
    return cb(10, 20);
}

@method_id(109)
fun test109(bigCost: bool) {
    return bigCost ? ton("0.05") : (ton("0.001"));
}

@method_id(110)
fun test110(k: int) {
    val c: [int, int, int] = [k, 6, 7];
    return c.0 > 0 ? c.1 : c.2;
}

@method_id(111)
fun test111(g: bool) {
    return g ? (1, 2) : (3, 4);
}


@method_id(200)
fun test200(x: int) {
    return 10 > 3 ? 200 : 100;
}

@method_id(201)
fun test201(x: int) {
    return (x is slice) ? 200 : x;
}

@method_id(202)
fun test202(x: int) {
    x > 10 ? x : 0;
    x < 10 ? x! : -10;
    return x;
}

@method_id(203)
fun test203() {
    var xx = false;
    if (!xx) {
        return xx ? 100 : 200;
    }
    return 300;
}


@method_id(220)
fun test220(p: Point?, getNull: bool) {
    if (p == null) {
        return getNull ? p : 20;
    }
    return p.y;
}

@method_id(221)
fun test221(c: (int, (int,int)?)) {
    if (c.1 != null) {
        return c.0 > 5 ? c.1.0 : c.1.1;
    }
    return c.0 > 5 ? 0 : c.1;
}


@method_id(300)
fun test300(x: int): tuple | int {
    // no condsel, because more that 1 slot
    return x > 0 ? x : gTup;
}

@method_id(301)
fun test301(first: bool) {
    var opt1 = (10, 20);
    var opt2 = (30, 40);
    // no condsel, because not 1 slot arguments
    return first ? opt1 : opt2;
}

@method_id(302)
fun test302(x: int?) {
    // no condsel, because not trivial (unary operator)
    return x == null ? 0 : -x;
}

@method_id(303)
fun test303(first: bool) {
    gTup = createEmptyTuple();
    // no condsel, because not trivial
    return (first ? get1() : get2(), gTup);
}

@method_id(304)
fun test304(y: bool) {
    return y ? Point.create0NoInline().y : 5;
}

@method_id(305)
fun test305(y: bool) {
    return y ? Point.create0().y : 5;
}


fun main() {}

/**
@testcase | 101 | 0       | 1 1
@testcase | 101 | 5       | 2 2
@testcase | 101 | 9       | 2 1
@testcase | 102 | 8       | 8
@testcase | 102 | null    | 0
@testcase | 103 | -1      | -2
@testcase | 104 | 1 2     | -1
@testcase | 104 | 0 2     | (null)
@testcase | 105 | -1      | 10
@testcase | 106 |         | [ 1 ]
@testcase | 107 |         | 1 20
@testcase | 108 | 0       | 20
@testcase | 109 | -1      | 50000000
@testcase | 110 | 5       | 6
@testcase | 110 | -5      | 7
@testcase | 111 | -1      | 1 2
@testcase | 111 | 0       | 3 4

@testcase | 200 | 20      | 200
@testcase | 201 | 5       | 5
@testcase | 202 | 5       | 5
@testcase | 203 |         | 200

@testcase | 220 | null null 0 -1   | (null)
@testcase | 220 | null null 0 0    | 20
@testcase | 220 | 9 8 123 5        | 8
@testcase | 221 | 10 null null 0   | 0
@testcase | 221 | 3 null null 0    | (null)
@testcase | 221 | 10 3 4 100       | 3
@testcase | 221 | 3 3 4 100        | 4

@testcase | 300 | 5       | 5 1
@testcase | 301 | -1      | 10 20
@testcase | 302 | null    | 0
@testcase | 303 | -1      | 1 [ 1 ]
@testcase | 304 | -1      | 0
@testcase | 304 | 0       | 5
@testcase | 305 | 0       | 5
@testcase | 305 | 0       | 5

@fif_codegen
"""
  test101() PROC:<{
    DUP
    2 PUSHINT
    1 PUSHINT
    CONDSEL
    SWAP
    5 EQINT
    2 PUSHINT
    1 PUSHINT
    CONDSEL
  }>
"""

@fif_codegen
"""
  test102() PROC:<{             //  x
    DUP                         //  x x
    ISNULL                      //  x '1
    0 PUSHINT
    ROT                         //  '1 '3=0 x
    CONDSEL                     //  '2
  }>
"""

@fif_codegen
"""
  test103() PROC:<{
    -2 PUSHINT
    2 PUSHINT
    CONDSEL
  }>
"""

@fif_codegen
"""
  test105() PROC:<{             //  getX
    10 PUSHINT                  //  getX '3=10
    20 PUSHINT                  //  getX p.x=10 p.y=20
    CONDSEL                     //  '5
  }>
"""

@fif_codegen
"""
  helper106() PROC:<{
    $gTup GETGLOB
    SWAP
    CONDSEL
  }>
"""

@fif_codegen
"""
  helper107() PROC:<{
    NIP
    s1 s(-1) PUXC
    CONDSEL
  }>
"""

@fif_codegen
"""
  test108() PROC:<{
    CONT:<{
      MIN
    }>
    CONT:<{
      MAX
    }>
    CONDSEL
    10 PUSHINT
    20 PUSHINT
    ROT
    2 1 CALLXARGS
  }>
"""

@fif_codegen
"""
  test109() PROC:<{
    50000000 PUSHINT
    1000000 PUSHINT
    CONDSEL
  }>
"""

@fif_codegen
"""
  test200() PROC:<{
   DROP
   200 PUSHINT
  }>
"""

@fif_codegen
"""
  test201() PROC:<{
    // '2
  }>
"""

@fif_codegen
"""
  test202() PROC:<{
  }>
"""

@fif_codegen
"""
  test203() PROC:<{
    200 PUSHINT
  }>
"""

@fif_codegen
"""
  test220() PROC:<{
    s3 POP
    0 EQINT
    IFJMP:<{
      20 PUSHINT
      CONDSEL
    }>
    NIP
  }>
"""

@fif_codegen
"""
  test304() PROC:<{
    IF:<{
      Point.create0NoInline() CALLDICT
"""

@fif_codegen
"""
  test305() PROC:<{
    IF:<{
      0 PUSHINT
    }>ELSE<{
      5 PUSHINT
    }>
  }>
"""

*/
