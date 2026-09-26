fun takeInt(a: int) {}

fun alwaysThrows() {
    throw 123;
}

fun alwaysThrowsButReturns() {
    throw 123;
    return 123;
}

@method_id(101)
fun test1(x: int?) {
    if (x == null && x != null) {
        var y = x;
        __expect_type(y, "never");
        __expect_type(y!, "never");
        // `never` type is assignable to anything, flow won't reach this point
        var t: (int, int) = x;
        t = y;
        takeInt(x);
        var cb: (int) -> int = x;
        x as int?;
        x as (int, int)?;
        x as never;
        return x;
    }
    return 123;
}

fun bigThrow() { throw 1222222222222222222222222222222223; }

fun alwaysAssertFalse(): int {
    assert (false) throw 101;
}

@method_id(102)
fun test2(): int {
    try {
        __expect_type(alwaysAssertFalse, "() -> int");
        return alwaysAssertFalse();
    } catch (excNo) {
        return excNo;
    }
}

@noinline
fun abortWith42() {
    throw 42;
}

@method_id(103)
fun testPreservedNeverCall(cond: bool): int {
    var x = 0;
    if (cond) {
        abortWith42();
    } else {
        x = 1;
    }
    return x;
}


fun main() {
    __expect_type(test1, "(int?) -> int");
    __expect_type(alwaysThrows(), "never");
    __expect_type(alwaysThrowsButReturns(), "int");
    __expect_type(bigThrow, "() -> never");
}

/**
@testcase | 101 | null    | 123
@testcase | 102 |         | 101
@testcase | 103 | 0       | 1

@fif_codegen
"""
  bigThrow() PROC:<{ 
    1222222222222222222222222222222223 PUSHINT
    THROWANY
  }>
"""
 */
