struct Point {
    x: int8 = 10
    y: int8 = 20
}

fun main() {
    var zuko: [int32, Point, int] = [ 100, {}, 8 ];
    return zuko.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `[int32, Point, int]`
@stderr because element `shaped.2` of type `int` can't be serialized
 */
