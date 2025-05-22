type MIntN = MInt?;
type MInt = int;
type MInt_v2 = int;
type MVoid = void;
type Pair2_v1 = (int, int);
type Pair2_v2 = (MInt, MInt);
type MBool = bool;
type Tuple2Int = [int, int];

struct Point { x: int; y: int }
type PointAlias = Point;
type PointAlias2 = PointAlias;

fun rand(): uint256
    asm "RANDU256";

fun test1(x: MInt): MVoid {
    var y = x;
    var z: MInt = 2;
    __expect_type(x, "MInt");
    __expect_type(y, "MInt");
    __expect_type(z, "MInt");

    __expect_type(x + y, "int");
    __expect_type((x + y) as MInt, "MInt");
    __expect_type(x!, "MInt");
    __expect_type(~x, "int");
    __expect_type(x as int, "int");
    __expect_type(x as int8, "int8");
    __expect_type(rand() ? x : y, "MInt");
    __expect_type((x, 1, y), "(MInt, int, MInt)");

    __expect_type(rand() ? (1, 2) : (1, 2) as Pair2_v1, "(int, int)");
    __expect_type(rand() ? (1, 2) : (1, 2) as Pair2_v2, "(int, int)");
    __expect_type(rand() ? (1, 2) as Pair2_v1 : (1, 2), "Pair2_v1");
    __expect_type(rand() ? (1, 2) as Pair2_v1 : (1, 2) as Pair2_v2, "(int, int)");
    __expect_type(rand() ? (1, 2) as Pair2_v2 : (1, 2) as Pair2_v2, "Pair2_v2");

    __expect_type(!x, "bool");

    __expect_type(x as int?, "int?");
    __expect_type(x as MInt?, "MInt?");
    __expect_type(x as MIntN, "MIntN");

    __expect_type(PointAlias{x:0,y:0}, "Point");
    __expect_type(PointAlias2{x:0,y:0}, "Point");
    __expect_type(Point{x:0,y:0} as PointAlias, "PointAlias");

    if (x) { return; }
}

fun test2() {
    __expect_type(test1, "(MInt) -> void");
    __expect_type(test1(1), "void");
}

fun test3(x: MIntN, y: MInt?) {
    __expect_type(x, "MIntN");
    __expect_type(x!, "MInt");
    __expect_type(y!, "MInt");
    if (x != null) {
        __expect_type(x, "MInt");
    }
    __expect_type(x, "MInt?");
    var (z1, z2) = (x, x!, );
    __expect_type(z1, "MInt?");
    __expect_type(z2, "MInt");
}

@method_id(104)
fun test4(x: MIntN, y: MIntN) {
    if (x != null && y != null) {
        __expect_type(x, "MInt");
        __expect_type(x + y, "int");
        return x + y;
    }
    __expect_type(x, "MInt?");
    __expect_type(x!, "MInt");
    __expect_type(rand() ? x : y, "MInt?");
    __expect_type(rand() ? x : 0, "MInt?");
    __expect_type(rand() ? 0 : x, "int?");
    __expect_type(rand() ? 0 : x!, "int");
    return y!;
}

fun takeTensor_v1(v: Pair2_v1) { return v.0 + v.1; }
fun takeTensor_v2(v: Pair2_v2) { return v.0 + v.1; }
fun takeTensor_v3(v: (int, int)) { return v.0 + v.1; }

fun test5() {
    var x: Pair2_v1 = (1, 2);
    var y = (3, 4) as Pair2_v2;
    var z = (5, 6);

    __expect_type(x, "Pair2_v1");
    __expect_type(y, "Pair2_v2");

    takeTensor_v1(x); takeTensor_v2(x); takeTensor_v3(x);
    takeTensor_v1(y); takeTensor_v2(y); takeTensor_v3(y);
    takeTensor_v1(z); takeTensor_v2(z); takeTensor_v3(z);

    var t = (y, x);
    return t.0.1 + t.1.0;
}

fun test6() {
    var (x1: MInt?, x2: MIntN) = (5, 5);    // smart cast
    __expect_type(x1, "MInt");
    __expect_type(x2, "MInt");
    var (y1: MInt?, y2: MIntN) = (null, null);
    __expect_type(y1, "null");
    __expect_type(y2, "null");
    var z: Pair2_v2? = (5, 5);
    __expect_type(z, "Pair2_v2");
    __expect_type(z.0, "MInt");
}

fun someFn1(v: MInt): MIntN { return v; }

fun test7() {
    var f1: (int) -> int? = someFn1;
    var f2: (int) -> MInt? = someFn1;
    var f3: (MInt_v2) -> MInt_v2? = someFn1;

    f1 = f2; f1 = f3;
    f2 = f1; f2 = f3;
    f3 = f1; f3 = f2;
}

fun test8() {
    0 as MInt;
    0 as MInt?;
    0 as MIntN;

    (1, 2) as Pair2_v2;
    (1, 2) as Pair2_v2?;
    (((1, 2) as Pair2_v2?) as (int, int)?) as Pair2_v1?;

    someFn1 as (int) -> int?;
    someFn1 as (int) -> MInt?;
    someFn1 as (MInt_v2) -> MInt_v2?;
}

fun test9(b: MBool): MBool {
    if (!b) {
        __expect_type(b, "MBool");
        return !b;
    }
    return !!(5 as MInt);
}

@method_id(110)
fun test10() {
    var x1: Pair2_v1 = (5, 6);
    var (a1, b1) = x1;
    __expect_type(a1, "int");
    var x2: Pair2_v2? = x1;
    var (a2, b2) = x2;
    __expect_type(a2, "MInt");
    var x3: Tuple2Int = [9, 10];
    var [a3, b3] = x3;
    return a1 + a2 + a3 + b1 + b2 + b3;
}

fun analyzeTensor1<T1, T2>(a: (T1, T2)): (T1, T2) { return a; }
fun analyzeTensor2<T1, T2>(a: (T1, T2)?): (T1, T2)? { return a; }

fun test11() {
    var (x1: (int8, int16), x2: Pair2_v1, x3: Pair2_v2) = ((1,2), (3,4), (5,6));
    __expect_type(analyzeTensor1(x1), "(int8, int16)");
    __expect_type(analyzeTensor1(x2), "(int, int)");
    __expect_type(analyzeTensor1(x3), "(MInt, MInt)");
    __expect_type(analyzeTensor2(x1), "(int8, int16)?");
    __expect_type(analyzeTensor2(x2), "(int, int)?");
    __expect_type(analyzeTensor2(x3), "(MInt, MInt)?");
}


fun main(x: MInt, y: MInt?) {
    return y == null ? x : x + y;
}

/**
@testcase | 0   | 3 4 | 7
@testcase | 104 | 1 2 | 3
@testcase | 110 |     | 41

@fif_codegen
"""
  test9 PROC:<{ //  b
    DUP	        //  b b
    IFNOTJMP:<{	//  b
      NOT	//  '2
    }>	    //  b
    DROP	//
    TRUE	//  '5
  }>
"""
 */
