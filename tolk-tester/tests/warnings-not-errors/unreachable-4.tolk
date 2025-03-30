fun alwaysThrows(): never {
    throw 456;
}

fun testUnreachable(x: int) {
    if (x) { throw 123; }
    else { alwaysThrows(); }
    return 1;
}

fun main() {
    try {
        testUnreachable(100);
        throw 80;
    } catch (excNo) {
        return excNo;
    }
}

/**
@testcase | 0 |   | 123
@stderr warning: unreachable code
@stderr return 1;
 */
