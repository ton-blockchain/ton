import "@stdlib/reflection"

struct Point {
    x: int8;
    y: int8;
}

fun main() {
    return reflect.serializationPrefixOf<Point>();
}

/**
@compilation_should_fail
@stderr type `Point` does not have a serialization prefix
 */
