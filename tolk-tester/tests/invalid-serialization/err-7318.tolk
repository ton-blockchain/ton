struct Point {
    x: int32;
    y: int32;
}

fun loadPoint(): Point {
    if (random.uint256() != 0) {
        return Point.fromSlice(createEmptySlice());
    }
    return Point.fromSlice(createEmptySlice());
}

fun main() {
    val p = lazy loadPoint();
    return p.x;
}

/**
@compilation_should_fail
@stderr `lazy` operator can only be used with built-in functions like fromCell/fromSlice or simple wrappers over them
 */
