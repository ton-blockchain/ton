struct Point { x: int, y: int }
fun Point.create() {}

fun nnn() {}

fun main() {
    Point.create = nnn;
}

/**
@compilation_should_fail
@stderr invalid left side of assignment
 */
