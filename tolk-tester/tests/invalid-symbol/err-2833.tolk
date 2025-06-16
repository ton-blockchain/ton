struct Point {
    x: int8;
    y: int8;
}

fun main() {
    return Point.getDeclaredPackPrefix();
}

/**
@compilation_should_fail
@stderr type `Point` does not have a serialization prefix
 */
