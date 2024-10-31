fun one(dummy: tuple) {
    return 1;
}

fun main(a: int, x: int) {
    var y: int = 0;
    var z: int = 0;
    while ((y = x * x) > a) {
        x -= 1;
        z = one(null);
    }
    return (y, z);
}

fun throwIfLt10(x: int): void {
    if (x > 10) {
        return;
    }
    throw 234;
    return;
}

@method_id(88)
fun test88(x: int) {
    try {
        var x: void = throwIfLt10(x);
        return 0;
    } catch(code) {
        return code;
    }
}

/**
    method_id | in     | out
@testcase |  0 | 101 15 | 100 1
@testcase |  0 | 101 14 | 100 1
@testcase |  0 | 101 10 | 100 0
@testcase |  0 | 100 10 | 100 0
@testcase |  0 | 100 10 | 100 0
@testcase | 88 | 5      | 234
@testcase | 88 | 50     | 0
*/
