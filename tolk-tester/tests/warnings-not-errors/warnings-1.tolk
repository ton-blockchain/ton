fun getNullableInt(): int? { return null; }

fun main() {
    var c: int? = 6;
    __expect_type(c, "int");
    if (c == null) {}

    var d: int? = c;
    if (((d)) != null && createEmptyTuple().size()) {}

    var e: int? = getNullableInt();
    if (e != null) {
        return true;
    }
    __expect_type(e, "null");
    null == e;

    return null != null;
}

/**
@testcase | 0 |    | 0

@stderr warning: variable `c` of type `int` can never be `null`, this condition is always false
@stderr warning: variable `d` of type `int` can never be `null`, this condition is always true
@stderr warning: variable `e` is always `null`, this condition is always true
@stderr warning: expression is always `null`, this condition is always false
 */
