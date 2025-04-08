fun main() {
    var (a, b, c, d, e) = (1, beginCell(), beginCell().endCell().beginParse(), [1], true as bool?);

    var alwaysInt = a != null ? 1 : null;
    __expect_type(alwaysInt, "int");

    if (!(c == null)) {
        if (10 < 3) { assert(b == null, 100); }
    }
    while (d == null || false) {}

    return e! != null;
}

/**
@testcase | 0 |    | -1

@stderr warning: variable `a` of type `int` can never be `null`, this condition is always true
@stderr warning: condition of ternary operator is always true
@stderr warning: variable `c` of type `slice` can never be `null`, this condition is always false
@stderr warning: condition of `if` is always true
@stderr warning: variable `b` of type `builder` can never be `null`, this condition is always false
@stderr warning: condition of `assert` is always false
@stderr warning: condition of `while` is always false
@stderr warning: expression of type `bool` can never be `null`, this condition is always true
 */
