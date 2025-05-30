struct Point {
    x: int;
    y: int;
}

fun main(p: Point) {
    p.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `Point`
@stderr because field `Point.x` of type `int` can't be serialized
@stderr because type `int` is not serializable, it doesn't define binary width
@stderr hint: replace `int` with `int32` / `uint64` / `coins` / etc.
 */
