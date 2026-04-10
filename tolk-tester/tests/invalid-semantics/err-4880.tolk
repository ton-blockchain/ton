fun f<X>(v: int, x: X) {}

fun failCantDeduceWithoutArgument() {
    return f(1);
}

/**
@compilation_should_fail
@stderr too few arguments in call to `f<X>`, expected 2, have 1
 */
