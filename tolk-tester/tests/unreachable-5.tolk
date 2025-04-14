fun main(x: int | slice | null) {
    if (x is builder) {
        __expect_type(x, "never");
        return 10;
    }
    if (x !is cell) {
        __expect_type(x, "int | slice | null");
    }
    if (x is int) {
        return x;
    }
    return (x is null) ? -1 : 20;
}

/**
@testcase | 0 | 0 1    | 0
@testcase | 0 | null 0 | -1
@testcase | 0 | -1 2   | 20

@stderr warning: variable `x` of type `int | slice | null` can never be `builder`
@stderr if (x is builder)
@stderr warning: variable `x` of type `int | slice | null` can never be `cell`
@stderr if (x !is cell)
 */
