struct Point {
    x: int8
    y: int8
}

fun main(s: slice): int {
    // lazy declaration in match subject is only allowed when `match` is a statement
    return 1 + match (val p = lazy Point.fromSlice(s)) {
        Point => p.x
    };
}

/**
@compilation_should_fail
@stderr error: incorrect `lazy` operator usage, it's not directly assigned to a variable
 */
