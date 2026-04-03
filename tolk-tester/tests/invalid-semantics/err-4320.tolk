import "@stdlib/reflection"

struct Point {
    x: int;
    y: int;
}

fun main() {
    return reflect.stackSizeOfObject(Point);
}

/**
@compilation_should_fail
@stderr `Point` only refers to a type, but is being used as a value here
 */
