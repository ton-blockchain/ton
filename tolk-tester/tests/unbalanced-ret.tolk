fun main(x: int): (int, int) {
    var y: int = 5;
    if (x < 0) {
        x *= 2;
        y += 1;
        if (x == -10) {
            return (111, 0)
        }
    }
    return (x + 1, y)
}

@inline
fun foo1(x: int): int {
    if (x < 0) {
        x *= 2;
        if (x == -10) {
            return 111;
        }
    }
    return x + 1;
}

@method_id(101)
fun test1(x: int): int {
    return foo1(x) * 10;
}

fun foo2(y: int): int {
    if (y < 0) {
        y *= 2;
        if (y == -10) {
            return 111;
        }
    }
    return y + 1;
}

fun bar2(x: int, y: int): (int, int) {
    if (x < 0) {
        y = foo2(y);
        x *= 2;
        if (x == -10) {
            return (111, y)
        }
    }
    return (x + 1, y);
}

@method_id(102)
fun test2(x: int, y: int): (int, int) {
    (x, y) = bar2(x, y);
    return (x, y * 10);
}

/**
      method_id | in    | out
@testcase | 0   | 10    | 11 5
@testcase | 0   | -5    | 111 0
@testcase | 0   | -4    | -7 6
@testcase | 101 | 10    | 110
@testcase | 101 | -5    | 1110
@testcase | 101 | -4    | -70
@testcase | 102 |  3  3 | 4 30
@testcase | 102 |  3 -5 | 4 -50
@testcase | 102 |  3 -4 | 4 -40
@testcase | 102 | -5  3 | 111 40
@testcase | 102 | -5 -5 | 111 1110
@testcase | 102 | -5 -4 | 111 -70
@testcase | 102 | -4  3 | -7 40
@testcase | 102 | -4 -5 | -7 1110
@testcase | 102 | -4 -4 | -7 -70
*/
