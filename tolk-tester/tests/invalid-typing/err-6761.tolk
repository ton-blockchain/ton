struct Point {
    x: int8
    y: int32
}


fun main() {
    var m = createEmptyMap<Point, address>();
    m.set(123, createAddressNone());
}

/**
@compilation_should_fail
@stderr can not pass `int` to `Point`
 */
