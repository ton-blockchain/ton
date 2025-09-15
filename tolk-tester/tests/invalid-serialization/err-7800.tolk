struct Point {
    x: int32;
    y: int32;
}

fun loadPoint(s: slice): Point {
    return Point.fromSlice(s);
}

fun main() {
    var p = lazy loadPoint(stringHexToSlice("0000000100000002"));
}

/**
@compilation_should_fail
@stderr `lazy` operator can only be used with built-in functions like fromCell/fromSlice
 */
