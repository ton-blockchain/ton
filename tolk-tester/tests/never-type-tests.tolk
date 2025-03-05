fun takeInt(a: int) {}

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

fun main() {
    __expect_type(test1, "(int?) -> int");
}

/**
@testcase | 101 | null    | 123
 */
