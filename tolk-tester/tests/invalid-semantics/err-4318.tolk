struct Point {
    x: int
    y: int
}

fun Point.getX() {
    return self.x
}

/**
@compilation_should_fail
@stderr using `self` in a static method
 */
