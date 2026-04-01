struct Point {
    x: int
    y: int
}

fun main() {
    var (a, b) = (0, 0);
    Point {x: a, y: b}.x = 100;
}

/**
@compilation_should_fail
@stderr can not assign to a temporary expression
 */
