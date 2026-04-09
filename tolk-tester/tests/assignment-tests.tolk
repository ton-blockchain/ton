fun extractFromTypedTuple(params: [int]) {
    var [payload: int, ] = params;
    return payload + 10;
}

fun int.increment(mutate self): self {
    self += 1;
    return self;
}

@method_id(101)
fun test101(x: int) {
    var params = [x, ];
    __expect_type(params, "array<int>");
    return extractFromTypedTuple(params as unknown as [int]);
}

fun autoInferIntNull(x: int) {
    if (x > 10) { return null; }
    return x;
}

fun typesAsIdentifiers(builder: builder) {
    var int = 1;
    var cell = builder.endCell();
    var slice = cell.beginParse();
    {
        var cell: cell = cell;
        var tuple: array<int> = createEmptyTuple() as array<int>;
        var bool: bool = tuple.get(0) > 0;
    }
    return int;
}

global callOrder: tuple;

fun getTensor_12() {
    callOrder.push(100);
    return (1, 2);
}
fun getTensor_1X(x: int) {
    callOrder.push(101);
    return (1, x);
}
fun getTuple_12(): [int, int] {
    callOrder.push(110);
    return [1, 2, ];
}
fun getTuple_1X(x: int): [int, int] {
    callOrder.push(111);
    return [1, x];
}
fun getUntypedTuple_12() {
    callOrder.push(120);
    var t = createEmptyTuple(); t.push(1); t.push(2);
    return t;
}
fun getUntypedTuple_1X(x: int) {
    callOrder.push(121);
    var t = createEmptyTuple(); t.push(1); t.push(x);
    return t;
}
fun getIntValue5() {
    callOrder.push(10);
    return 5;
}
fun getIntValueX(x: int) {
    callOrder.push(11);
    return x;
}

global g108: int;
fun assertEq(a: int, b: int) {
    assert(a == b, 10);
    return b;
}

global g110: int;
global t110: (int, int);

@method_id(112)
fun test112() {
    var xy = [1, 2] as [coins, coins];
    ((((xy))).0, ((xy.1))) = ((xy).1, ((xy.0)));
    return xy;
}

@method_id(113)
fun test113() {
    var (a, t, z, ) = (1, [2,3] as [uint8, uint16], (-1,-1));
    (a, t.0, z.0, t.1, z.1) = (13, a += 1, a, 14, t.1);
    return (a, t, z);
}

global g114: int;
global t114: [int, int];
global z114: (int, int);

@method_id(114)
fun test114() {
    g114 = 1;
    t114 = [2, 3];
    (g114, t114.0, t114.1, z114) = (13, g114 += 1, 14, (g114, t114.1));
    return (g114, t114, z114);
}

@method_id(116)
fun test116() {
    var (a,b,c,d) = (0,0,0,0);
    var rhs = [1, 2, 3, 4] as [int,int,int,int];
    var rhs2 = ([a,b,c,d] = rhs);
    __expect_type(rhs2, "[int, int, int, int]");
    return (a, b, c, d, rhs2);
}

@method_id(117)
fun test117() {
    var c = [((1, ), ), ];
    __expect_type(c, "array<int>");
    c.set(10, 0);
    c.set((20, ), 0);
    return c;
}

struct Storage {
    owner: slice;
}

@method_id(118)
fun test118(x: int) {
    var i1: int;
    if (10 > 3) {
        i1 = 10;
    } else { { (_, i1) = (5, 20); } }

    var i2: int10;
    match (x) {
        1 => i2 = 1,
        2 => i2 = 2,
        3 => i2 = 3,
        else => i2 = 4,
    }

    var st: Storage?;
    if (x > 100) {
        throw 123;
    } else {
        if (x < -100) { throw 456; }
        else { st = { owner: "".beginParse() }}
    }

    var i3: (int, int) | builder;
    match (i1) {
        int => i3 = (1, 2),
    }

    var unused: int8;

    return (i1, i2, st.owner.remainingBitsCount(), i3.0);
}

@method_id(119)
fun test119(a: int, b: int) {
    var t: [unknown, unknown] = [a + 1, b * 2];
    __expect_type(t.1, "unknown");
    val l = (t as unknown as tuple).size();
    val [c,d:int] = [t.0 as int, t.1 as int];
    return (l, c, d);
}

struct Point {
    x: int
    y: int
}

struct ROLine {
    readonly from: Point
    readonly to: Point
}

fun eq<T>(v: T) { return v }

global i122: int;
global ab122: (int, int);

@method_id(122)
fun test122() {
    (i122, ab122) = (0, (0,0));
    (i122, ab122.0).1 = 10;     // modifies ab122.0
    return (i122, ab122);
}

@method_id(123)
fun test123() {
    var a = 0;
    a = a = a = 10;
    a = a.increment();
    (a, 0).0 = a.increment().increment();
    return a;
}

// now globals can have more than 15 items (they are tupled)
global g124: (int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int);

@method_id(124)
fun test124() {
    g124 = (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18);
    var (a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,a17,a18) = g124;
    return (a1, a18);
}


fun main(value: int, ) {
    var (x: int?, y,) = (autoInferIntNull(value), autoInferIntNull(value * 2));
    if (x == null && y == null) { return null; }
    return x == null || y == null ? -1 : x! + y!;
}

/**
@testcase | 0   | 3      | 9
@testcase | 0   | 6      | -1
@testcase | 0   | 11     | (null)
@testcase | 101 | 78     | 88
@testcase | 112 |        | [ 2 1 ]
@testcase | 113 |        | 13 [ 2 14 ] 2 3
@testcase | 114 |        | 13 [ 2 14 ] 2 3
@testcase | 116 |        | 1 2 3 4 [ 1 2 3 4 ]
@testcase | 117 |        | [ 20 ]
@testcase | 118 | 3      | 10 3 0 1
@testcase | 119 | 1 2    | 2 2 4
@testcase | 122 |        | 0 10 0
@testcase | 123 |        | 13
@testcase | 124 |        | 1 18


@fif_codegen
"""
  test123() PROC:<{
    13 PUSHINT
  }>
"""

@fif_codegen
"""
  test116() PROC:<{ //
    1 PUSHINT	    //  '9=1
    2 PUSHINT	    //  '9=1 '10=2
    3 PUSHINT	    //  '9=1 '10=2 '11=3
    4 PUSHINT	    //  '9=1 '10=2 '11=3 '12=4
    4 TUPLE	        //  rhs
    DUP	            //  rhs rhs
    4 UNTUPLE	    //  rhs2 a b c d
    4 ROLL	        //  a b c d rhs2
  }>
"""
*/
