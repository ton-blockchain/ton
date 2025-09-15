struct Point {
    x: int8;
    y: int8;
}

fun neg11(s: slice) {
    // mutating function can't be lazy
    var p = lazy s.loadAny<Point>();
    return p.x;
}

/**
@compilation_should_fail
@stderr `lazy` operator can only be used with built-in functions like fromCell/fromSlice or simple wrappers over them
*/
