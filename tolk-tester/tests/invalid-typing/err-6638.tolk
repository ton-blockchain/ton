struct Point {
    x: int = 10
    y: int = 20
}

fun int.increment(mutate self) {
    self += 1
}

fun main(p: Point) {
    (p = {}).x.increment();
}

/**
@compilation_should_fail
@stderr can not mutate a temporary expression
 */
