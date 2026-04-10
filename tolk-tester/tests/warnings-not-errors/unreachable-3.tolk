fun main(x: int?) {
    if (x != null && x == null) {
        return 1 + 2;
    }
    if (x == null) {
        return -1;
    }
    if (x != null) {
        return -2;
    }
    return 3 + 4;
}

/**
@testcase | 0 | 5    | -2
@testcase | 0 | null | -1

@stderr warning: variable `x` of type `int` can never be `null`, this condition is always true
@stderr if (x != null)
@stderr warning: unreachable code
@stderr return 3 + 4
 */
