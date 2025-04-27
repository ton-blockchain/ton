fun main() {
    var x = 0 as int | slice;
    if (x !is builder) {}
    while ((x = x) is builder) {}

    match (val tt = x) {
        int => {
            assert (tt is int, 505);
        }
        slice => {}
    }
    assert(x is int) throw 400;
    __expect_type(x, "int");
    return x == null;
}

/**
@testcase | 0 |    | 0

@stderr warning: variable `x` of type `int | slice` can never be `builder`, this condition is always true
@stderr warning: condition of `if` is always true
@stderr warning: expression of type `int | slice` can never be `builder`, this condition is always false
@stderr warning: condition of `while` is always false
@stderr warning: variable `tt` is always `int`, this condition is always true
@stderr warning: condition of `assert` is always true
 */
