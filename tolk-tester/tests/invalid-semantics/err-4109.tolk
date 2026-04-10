struct Wrapper<T> {
    value: T;
}

fun wrap0<T>(): Wrapper<T> {
    return { value: 0 }
}

fun main() {
    var m: Wrapper<int> | Wrapper<int8> = wrap0();
}

/**
@compilation_should_fail
@stderr can not deduce T for generic function `wrap0<T>`
@stderr instantiate it manually with `wrap0<...>()`
 */
