fun f<X>(v: int, x: X) {}

fun failCantDeduceWithoutArgument() {
    return f(1);
}

/**
@compilation_should_fail
@stderr can not deduce X for generic function `f<X>`
 */
