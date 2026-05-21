struct Box<T> {
    next: Box<T>
}

fun makeBox<T>(): Box<T> {
    throw 101;
}

fun main() {
    return makeBox<int>();
}

/**
@compilation_should_fail
@stderr struct `Box<int>` size is infinity due to recursive fields
 */
