fun f<T>(v: int, x: T) {}

fun failCantDeduceWithPlainNull() {
    return f(0, null);
}

/**
@compilation_should_fail
@stderr can not deduce T for generic function `f<T>`
 */
