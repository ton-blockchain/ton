struct ROPoint {
    readonly x: int
    readonly y: int
}

fun increment(mutate x: int) {}

fun main(p1: ROPoint, p2: ROPoint) {
    increment(mutate (p1 = p2).x);
}

/**
@compilation_should_fail
@stderr modifying readonly field `ROPoint.x`
 */
