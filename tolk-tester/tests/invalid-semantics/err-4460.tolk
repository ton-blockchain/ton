struct Point {
    x: int8;
    y: int8;
}

fun main() {
    var p = lazy Point.fromSlice(stringHexToSlice("0A14"));
    p.asdf;
}

/**
@compilation_should_fail
@stderr field `asdf` doesn't exist in type `Point`
 */
