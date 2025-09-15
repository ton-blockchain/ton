struct Point {
    x: int8;
    y: int8;
}

fun main() {
    var p = Point.fromSlice(stringHexToSlice("0102"));
    p.forceLoadLazyObject();
}

/**
@compilation_should_fail
@stderr this method is applicable to lazy variables only
 */
