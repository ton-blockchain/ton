fun main(a: int, b: int, c: int, d: int, e: int, f: int): (int, int) {
    var D: int = a * d - b * c;
    var Dx: int = e * d - b * f;
    var Dy: int = a * f - e * c;
    return (Dx / D, Dy / D);
}

@method_id(101)
fun testDivMod(x: int, y: int) {
    return (divMod(x, y), modDiv(x, y), mulDivMod(x, y, 10));
}

@deprecated
fun twice(f: (int) -> int, x: int) {
    return f (f (x));
}

fun sqr(x: int) {
    return x * x;
}

@method_id(102)
fun testCallVar(x: int): int {
    var f = sqr;
    return twice(f, x) * f(x);
}

@method_id(103)
fun testCallDirect(x: int): int {
    return twice(sqr, x) * sqr(x);
}

@method_id(104)
fun testLoop(x: int): int {
    var n = 0;
    while (x > 1) {
        n += 1;
        if (x & 1) {
            x = 3 * x + 1;
        } else {
            x >>= 1;
        }
    }
    return n;
}

@method_id(105)
fun test5(id: int): (int, int) {
    if (id > 0) {
        if (id > 10) {
            return (2 * id, 3 * id);
        }
    }
    return (5, 6);
}

@method_id(106)
fun test6(x: int): int {
    var i: int = 0;
    do {
        i = i + 1;
        if (i > 5) {
            return 1;
        }
        var f: bool = (i * i == 64);
    } while (!f);
    return -1;
}

@method_id(107)
fun test7(y: int): int {
    var x: int = 1;
    if (y > 0) {
        return 1;
    }
    return x > 0 ? -1 : 0;
}

@method_id(108)
fun test8(y: int): int {
    if (y > 0) {
        return 1;
    }
    return 2;
}

@method_id(109)
fun test9(s: int) {
    var (z, t) = (17, s);
    while (z > 0) {
        t = s;
        z -= 1;
    }
    return ~ t;
}


/**
    method_id   | in            | out
@testcase | 0   | 1 1 1 -1 10 6 | 8 2
@testcase | 0   | 817 -31 624 -241 132272 272276 | 132 -788
@testcase | 0   | -886 562 498 -212 -36452 -68958 | -505 -861
@testcase | 0   | 448 -433 -444 792 150012 -356232 | -218 -572
@testcase | 0   | -40 -821 433 -734 -721629 -741724 | -206 889
@testcase | 0   | -261 -98 -494 868 -166153 733738 | 263 995
@testcase | 101 | 112 3 | 37 1 1 37 33 6
@testcase | 102 | 3   | 729
@testcase | 102 | 10  | 1000000
@testcase | 103 | 3   | 729
@testcase | 103 | 10  | 1000000
@testcase | 104 |   1 | 0
@testcase | 104 |   2 | 1
@testcase | 104 |   5 | 5
@testcase | 104 |  19 | 20
@testcase | 104 |  27 | 111
@testcase | 104 | 100 | 25
@testcase | 105 | 0   | 5 6
@testcase | 105 | 4   | 5 6
@testcase | 105 | 11  | 22 33
@testcase | 106 | 0   | 1
@testcase | 107 | 10  | 1
@testcase | 107 | -5  | -1
@testcase | 108 | 10  | 1
@testcase | 108 | -5  | 2
@testcase | 109 | 1   | -2
@testcase | 109 | 5   | -6
*/
