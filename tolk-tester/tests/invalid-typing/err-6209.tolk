struct Point {
    x: int
    y: int
}

type c = [Point]

fun main(c: tuple, x: int, y: int) {
    array<c> [
        [{x, y}],       // ok
        [{x, y} as Point?]
    ];
}

/**
@compilation_should_fail
@stderr can not convert `[Point?]` to `c`
 */
