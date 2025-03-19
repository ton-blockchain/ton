type TensorOf2IntNull = (int?, int?);
type MInt = int;
type MIntN = int?;

fun getNullableInt(): int? { return 5; }

fun sumOfNullableTensorComponents(t: (int, int)?): int {
    if (t == null) { return 0; }
    return t!.0 + t!.1;
}

fun isTensorNull(t: (int, int)?) {
    return t == null;
}

fun (int, int)?.isTensorNull(self) {
    return self == null;
}

fun incrementNullableTensorComponents(mutate t: (int, int)?) {
    if (t != null) {
        t!.0 += 1;
        t!.1 += 1;
    }
}

fun (int, int)?.incrementNullableTensorComponents(mutate self): self {
    if (self != null) {
        self!.0 += 1;
        self!.1 += 1;
    }
    return self;
}

fun incrementTensorComponents(mutate t: (int, int)) {
    t.0 += 1;
    t.1 += 1;
}

fun (int, int).incrementTensorComponents(mutate self): self {
    self.0 += 1;
    self.1 += 1;
    return self;
}

fun assignFirstComponent(mutate t: (int, int), first: int) {
    t!.0 = first;
}

fun assignFirstComponentNullable(mutate t: (int, int)?, first: int) {
    if (t == null) {
        t = (first, 0);
    } else {
        t!.0 = first;
    }
}

fun getNullableTensor(firstComponent: int?): (int, int)? {
    return firstComponent == null ? null : (firstComponent!, 2);
}

fun sumOfTensor(x: (int, int)) {
    return x.0 + x.1;
}

fun assignNullTo<T>(mutate x: T?) {
    x = null;
}

fun getTensor12() {
    return (1,2);
}

@method_id(101)
fun test101(): (int, int)? {
    return (1, 2);
}

@method_id(102)
fun test102(): ((int, int)?, (int, int)?) {
    var t = (1, 2);
    return (t, null);
}

@method_id(103)
fun test103(t: (int, int)) {
    var t2: (int, int)? = t;
    return (sumOfNullableTensorComponents(t), sumOfNullableTensorComponents(t2), sumOfNullableTensorComponents(null), t2);
}

@method_id(104)
fun test104() {
    var t1_1: (int, int)? = (1, 2);
    var t1_2: (int, int)? = t1_1;
    var t1_3: (int, int)? = t1_1!;
    var t2_1: (int, int)? = getNullableTensor(null);
    var t2_2 = t2_1;
    return (t1_3, t2_2);
}

@method_id(105)
fun test105() {
    return (null as (int, slice, cell)?, (1, 2, 3) as (int, int, int)?);
}

@method_id(106)
fun test106() {
    var t: (int?, int?)? = (((((1, 2))) as (int, int)));
    return t;
}

@method_id(107)
fun test107() {
    var ab = (1, 2);
    var ab2: (int, int)? = ab;
    return (isTensorNull(ab), isTensorNull(ab2), isTensorNull(null), ab.isTensorNull(), ab2.isTensorNull(), null.isTensorNull());
}

@method_id(108)
fun test108(x1: (int, int)) {
    incrementTensorComponents(mutate x1);
    x1.incrementTensorComponents();
    var x2: (int, int)? = x1;
    __expect_type(x2, "(int, int)");
    x2.incrementNullableTensorComponents().incrementNullableTensorComponents();
    incrementNullableTensorComponents(mutate x2);
    __expect_type(x2, "(int, int)?");
    var x3: (int, int)? = null;
    __expect_type(x3, "null");
    x3.incrementNullableTensorComponents().incrementNullableTensorComponents();
    incrementNullableTensorComponents(mutate x3);
    return (x1, x2, x3);
}

fun isTensorNullGen<T1, T2>(t: (T1, T2)?) {
    return t == null;
}

fun (T1, T2)?.isTensorNullGen(self) {
    return self == null;
}

@method_id(109)
fun test109() {
    var x1 = (1, 2);
    var x2: (int, int)? = x1;
    var x3: (int, int)? = x1.1 > 10 ? (1, 2) : null;
    return (
        isTensorNullGen(x1), isTensorNullGen(x2), isTensorNullGen<int,int>(null),
        isTensorNullGen<int,int>(x1), isTensorNullGen<int,int>(x3),
        x1.isTensorNullGen(), x2.isTensorNullGen(), x3.isTensorNullGen(), (null as (int,int)?).isTensorNullGen()
    );
}

global g110_1: (int, int);
global g110_2: (int, int)?;

@method_id(110)
fun test110() {
    g110_1 = getNullableTensor(1)!;
    incrementTensorComponents(mutate g110_1);
    g110_1.incrementTensorComponents();
    g110_2 = g110_1;
    g110_2.incrementNullableTensorComponents().incrementNullableTensorComponents();
    incrementNullableTensorComponents(mutate g110_2);
    var tmp = g110_2;
    g110_2 = null;
    g110_2.incrementNullableTensorComponents();
    incrementNullableTensorComponents(mutate g110_2);
    return (g110_1, g110_2, tmp);
}

@method_id(111)
fun test111() {
    var x = (1, 2);
    assignFirstComponent(mutate x, 50);
    var x2: (int, int)? = null;
    var x3 = x2 as (int, int)?;
    assignFirstComponentNullable(mutate x2, 30);
    assignFirstComponentNullable(mutate x3, 70);
    g110_1 = (1, 2);
    g110_2 = null;
    assignFirstComponent(mutate g110_1, 90);
    assignFirstComponentNullable(mutate g110_2, 100);
    return (x.0, x2!.0, x3!.0, g110_1.0, g110_2!.0);
}

@method_id(112)
fun test112() {
    var x: (int, int)? = (10, 20);
    incrementTensorComponents(mutate x!);
    x!.incrementTensorComponents();
    return x;
}

@method_id(113)
fun test113() {
    var t = [1, null];      // t.1 is always null
    return isTensorNull(t.1);
}

@method_id(114)
fun test114(): ((slice, (cell, [int, slice, tuple]))?, slice?, (int?, bool?)?) {
    var t = [[null]];
    return (t.0.0, t.0.0, t.0.0);
}

@method_id(115)
fun test115() {
    var tt = getNullableTensor(null);
    assignFirstComponentNullable(mutate tt, 5);
    return (
        getNullableTensor(1)!.incrementTensorComponents(),
        sumOfNullableTensorComponents(getNullableTensor(1).incrementNullableTensorComponents().incrementNullableTensorComponents()),
        getNullableTensor(null).incrementNullableTensorComponents(),
        tt,
        sumOfNullableTensorComponents(getNullableTensor(null))
    );
}

@method_id(116)
fun test116(returnNull: bool) {
    var t1: (int, int)? = returnNull ? null : getTensor12();
    var t2 = returnNull ? null as (int, int)? : getTensor12() as (int, int)?;
    returnNull ? null : (1, 2);
    return (t1, t2);
}

@method_id(117)
fun test117() {
    var (a, b: (int, int)?, c) = (1, null, 3);
    return (b, a, c);
}

fun autoInferNullableTensor(a: int?, b: int) {
    if (a != null) {
        return (a!, b);
    }
    return null;
}

@method_id(118)
fun test118(a: int?) {
    return autoInferNullableTensor(a, 10);
}

@method_id(119)
fun test119() {
    var x: (int, int)? = (1, 2);
    x = null;
    var tt: (int, (int, int)?) = (0, (1, 2));
    tt.1 = null;
    var third: (int, (int, int)?, int) = (0, (1, 2), 3);
    third.2 = 100;
    return (x, tt.1, third.1, third.2);
}

@method_id(120)
fun test120(setNull: bool) {
    var x: (int, int)? = (1, 2);
    if (setNull) {
        assignNullTo(mutate x);
    }
    return x;
}

@method_id(121)
fun test121() {
    var t: [int?, [int?, int?]?] = [1, [2, 3]];
    t.1 = [3, 4];
    return t;
}

@method_id(122)
fun test122(setNull: bool) {
    var t: [int?, [int?, int?]?, int?, [int?, int?]?]? = [1, [2, 3], 4, null];
    if (setNull) {
        assignNullTo(mutate t!.1);
    } else {
        var rhs = [3, 4];
        t!!.1 = rhs;
    }
    return t;
}

@method_id(123)
fun test123() {
    var t: (int?, (int?, int?)?) = (1, (2, 3));
    t.1 = (3, 4);
    return t;
}

@method_id(124)
fun test124(setNull: bool) {
    var t: (int?, (int?, int?)?, int?, (int?, int?)?)? = (1, (2, 3), 4, null);
    if (setNull) {
        assignNullTo(mutate t!.1);
    } else {
        var rhs = (3, 4);
        t!!.1 = rhs;
    }
    return t;
}

global g125: int;
fun getT125(): (int, (int, int)?, (int?, int)?) { return (g125 += 1, null, null); }

@method_id(125)
fun test125() {
    g125 = 0;
    getT125().1 = null;
    getT125().2 = (1, 2);
    (getT125()!! as (int, (int, int)?, (int?, int)?)).2 = null;
    // test that nothing left on a stack
    return g125;
}

@method_id(126)
fun test126() {
    var tt1: (int, null, int) = (1, null, 2);
    var (a: int, b: (int, int)?, c: int) = tt1;
    return (a, b, c);
}

@method_id(127)
fun test127(choice: int) {
    var tt1: (int, null, int) = (1, null, 2);
    var tt2: (int, (int, int), int) = (1, (2, 3), 4);
    var tt3: (int, (int, int)?, int) = (1, null, 5);
    var abc: (int, (int, int)?, int) = choice == 1 ? tt1 : choice == 2 ? tt2 : tt3;
    return abc;
}

fun get128_1() { return (1, null, 2); }
fun get128_2() { return null; }
fun get128_3() { return (1, (2, 3), 4); }
fun takeT128(abc: (int, (int, int)?, int)?) { return abc; }

@method_id(128)
fun test128(choice: int) {
    if (choice == 1) {
        return takeT128(get128_1())!;
    }
    if (choice == 2) {
        return takeT128(get128_2());
    }
    return takeT128(get128_3());
}

@method_id(129)
fun test129(setNull: bool) {
    var t: (int?, int?) = (getNullableInt(), getNullableInt());
    var r1 = (t, t == null, t != null);
    t = (setNull ? null : 1, setNull ? null : 2);
    var r2 = (t, t == null, t != null);
    return (r1, r2);
}

@method_id(130)
fun test130(setNull: bool) {
    var os: (int, (int, int)?) = (1, setNull ? null : (2, 3));
    return os;
}

fun getEmptyNullableTensor(getNull: bool): ()? {
    return getNull ? null : ();
}

@method_id(131)
fun test131() {
    var nonNullEmptyT = getEmptyNullableTensor(false);
    var nullEmptyT = getEmptyNullableTensor(true);
    var emptyT = nonNullEmptyT!;
    __expect_type(emptyT, "()");
    var doubleNulls1 = (null, null) as (()?, ()?);
    var doubleNulls2 = ((), ()) as (()?, ()?);
    var doubleNulls3 = ((), ()) as (()?, ()?)?;
    var stillEmpty = ((), ());
    return (nonNullEmptyT, 777, nullEmptyT, 777, emptyT, 777, nullEmptyT!, 777, doubleNulls1, doubleNulls2, 777, doubleNulls3, 777, stillEmpty);
}

@method_id(132)
fun test132() {
    var doubleNulls: (()?, ()?) = (getEmptyNullableTensor(true), getEmptyNullableTensor(false));
    var result = ((null as ()?) == null, (() as ()?) == null, doubleNulls.0 == null, doubleNulls.1 == null);
    var aln1: int? = (doubleNulls.1 = null);
    var aln2: null = (doubleNulls.1 = null);
    return (result, 777, aln1, aln2, doubleNulls.1 == null, doubleNulls);
}

@method_id(133)
fun test133() {
    var x: (int, int)? = (10, 20);
    return sumOfTensor(x) + x.0 + x.1;     // smart casted
}

@method_id(134)
fun test134(): (int, int)? {
    var x: (int, int)? = (10, 20);
    incrementTensorComponents(mutate x);   // smart casted
    return x;
}


fun getNormalNullableTensorWidth1(vLess100: int?): ([int?], ())? {
    if (vLess100 != null && vLess100 >= 100) {
        return null;
    }
    return ([vLess100], ());    // such a nullable tensor can store NULL in the same slot
}

type TrickyActually2SlotTensor = (MIntN, ())?;

fun getTrickyNullableTensorWidth1(vLess100: int?): TrickyActually2SlotTensor {
    if (vLess100 != null && vLess100 >= 100) {
        return null;
    }
    return (vLess100, ());      // such a nullable tensor requires an extra stack slot for null presence
}

fun getEvenTrickierNullableWidth1(vLess100: int?): ((), (int?, ()), ())? {
    if (vLess100 != null && vLess100 >= 100) {
        return null;
    }
    return ((), (vLess100, ()), ());
}

@method_id(135)
fun test135() {
    var n1 = getNormalNullableTensorWidth1(10);         // ([10], ())
    var n2 = getNormalNullableTensorWidth1(null);       // ([null], ())
    var n3 = getNormalNullableTensorWidth1(100);        // null
    var t1 = getTrickyNullableTensorWidth1(10);         // (10, ())
    var t2 = getTrickyNullableTensorWidth1(null);       // (null, ())
    var t3 = getTrickyNullableTensorWidth1(100);        // null
    var e1 = getEvenTrickierNullableWidth1(10);         // ((), (10, ()), ())
    var e2 = getEvenTrickierNullableWidth1(null);       // ((), (null, (), ())
    var e3 = getEvenTrickierNullableWidth1(100);        // null
    return (n1, n2, n3, 777, t1, t2, t3, 777, e1, e2, e3, 777,
        n1 == null, n2 == null, n3 == null, t1 == null, t2 == null, t3 == null, e1 == null, e2 == null, e3 == null, 777,
        t1!.0 == null, t2!.0 == null, e1!.1.0 == null, e1!.1.1 == null, e2!.1.0 == null, e2!.1.1 == null);
}

@method_id(136, )
fun test136(x: int?, ) {
    var t1 = (x, );     // it's not a tensor with 1 element
    __expect_type(t1, "int?", );
    return ((t1, t1 == null, ), );   // testing trailing comma everywhere :)
}

@method_id(140)
fun test140(x: int8, y: int16) {
    var t1: (int32, int64)? = ((x, y) as (int32, int64)) as (int32, int64)?;
    var t2: (int32, int64)? = null as (int32, int64)?;
    return (t1, t2);
}

@method_id(141)
fun test141() {
    var t: TensorOf2IntNull = (1, 2);
    if (t.0 != null) {
        t.0 = null;
    }
    t.1 != null ? t.1 = 10 : t.1 = null;
    return t;
}

type T142 = (int, MInt)?;

@method_id(142)
fun test142() {
    var t = (1, 2) as T142;
    __expect_type(t!, "(int, MInt)");
    var sum1 = sumOfNullableTensorComponents(t);
    var sum2 = sumOfTensor(t!);
    return (sum1, sum2, t!);
}

@method_id(143)
fun test143(setBNull: bool, setANullMid: bool) {
    var a: (int, int | slice | null, int)? = (1, 2, 3);
    var b = setBNull ? null : a;
    if (setANullMid) {
        a.1 = null;
    }
    if (a.1 != null && b != null) {
        b.0 += match (a.1) { int => 100, slice => 200 };
    }
    __expect_type(a, "(int, int | slice | null, int)");
    __expect_type(b, "(int, int | slice | null, int)?");
    return (a, 777, b);
}

fun main(){}

/**
@testcase | 101 |      | 1 2 129
@testcase | 102 |      | 1 2 129 (null) (null) 0
@testcase | 103 | 1 2  | 3 3 0 1 2
@testcase | 104 |      | 1 2 (null) (null) 0
@testcase | 105 |      | (null) (null) (null) 0 1 2 3 131
@testcase | 106 |      | 1 2
@testcase | 107 |      | 0 0 -1 0 0 -1
@testcase | 108 | 5 6  | 7 8 10 11 129 (null) (null) 0
@testcase | 109 |      | 0 0 -1 0 -1 0 0 -1 -1
@testcase | 110 |      | 3 4 (null) (null) 0 6 7 129
@testcase | 111 |      | 50 30 70 90 100
@testcase | 112 |      | 12 22
@testcase | 113 |      | -1
@testcase | 114 |      | (null) (null) (null) 0 (null) (null) (null) 0
@testcase | 115 |      | 2 3 7 (null) (null) 0 5 0 129 0
@testcase | 116 | -1   | (null) (null) 0 (null) (null) 0
@testcase | 116 | 0    | 1 2 129 1 2 129
@testcase | 117 |      | (null) 1 3
@testcase | 118 | 5    | 5 10 129
@testcase | 118 | null | (null) (null) 0
@testcase | 119 |      | (null) (null) 1 2 129 100
@testcase | 120 | -1   | (null) (null) 0
@testcase | 120 | 0    | 1 2 129
@testcase | 121 |      | [ 1 [ 3 4 ] ]
@testcase | 122 | 0    | [ 1 [ 3 4 ] 4 (null) ]
@testcase | 122 | -1   | [ 1 (null) 4 (null) ]
@testcase | 123 |      | 1 3 4 132
@testcase | 124 | 0    | 1 3 4 132 4 (null) (null) 0
@testcase | 124 | -1   | 1 (null) (null) 0 4 (null) (null) 0
@testcase | 125 |      | 3
@testcase | 126 |      | 1 (null) 2
@testcase | 127 | 1    | 1 (null) (null) 0 2
@testcase | 127 | 2    | 1 2 3 129 4
@testcase | 127 | 3    | 1 (null) (null) 0 5
@testcase | 128 | 1    | 1 (null) (null) 0 2 139
@testcase | 128 | 2    | (null) (null) (null) (null) (null) 0
@testcase | 128 | 3    | 1 2 3 129 4 139
@testcase | 129 | 0    | 5 5 0 -1 1 2 0 -1
@testcase | 129 | -1   | 5 5 0 -1 (null) (null) 0 -1
@testcase | 130 | 0    | 1 2 3 129
@testcase | 130 | -1   | 1 (null) (null) 0
@testcase | 131 |      | 140 777 0 777 777 777 0 0 140 140 777 140 140 141 777
@testcase | 132 |      | -1 0 -1 0 777 (null) (null) -1 0 0
@testcase | 133 |      | 60
@testcase | 134 |      | 11 21 129
@testcase | 135 |      | [ 10 ] [ (null) ] (null) 777 10 143 (null) 143 (null) 0 777 10 144 (null) 144 (null) 0 777 0 0 -1 0 0 -1 0 0 -1 777 0 -1 0 0 -1 0
@testcase | 136 | 9    | 9 0
@testcase | 136 | null | (null) -1
@testcase | 140 | 8 9  | 8 9 145 (null) (null) 0
@testcase | 141 |      | (null) 10
@testcase | 142 |      | 3 3 1 2
@testcase | 143 | -1 0 | 1 2 1 3 777 (null) (null) (null) (null) 0
@testcase | 143 | 0 -1 | 1 (null) 0 3 777 1 2 1 3 146

@fif_codegen
"""
  isTensorNull PROC:<{  //  t.0 t.1 t.UTag
    2 1 BLKDROP2	    //  t.UTag
    0 EQINT	//  '3
  }>
"""

@fif_codegen
"""
  test113 PROC:<{
    1 PUSHINT	//  '2=1
    PUSHNULL	//  '2=1 '3
    PAIR	//  t
    1 INDEX	//  '5
    PUSHNULL	//  '5 '6
    0 PUSHINT	//  '5 '6 '7=0
    isTensorNull CALLDICT	//  '8
  }>
"""
*/
