struct Box<T> {
    value: T;
}

type Tagged<T> = Box<T>;

fun main(v: uint8) {
    return Tagged<uint8> { value: v };
}

/**
@compilation_should_fail
@stderr `Tagged<uint8>` does not name a struct
@stderr { value: v }
 */
