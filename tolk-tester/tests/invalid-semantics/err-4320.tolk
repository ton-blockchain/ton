struct Point {
    x: int;
    y: int;
}

fun main() {
    return sizeof(Point);
}

/**
@compilation_should_fail
@stderr `Point` only refers to a type, but is being used as a value here
 */
