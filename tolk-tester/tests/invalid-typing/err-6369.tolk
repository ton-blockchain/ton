fun f<T>(x: T, y: T) {}

fun failIncompatibleTypesForT() {
    return f(32, "");
}

/**
@compilation_should_fail
@stderr T is both int and slice for generic function `f<T>`
@stderr f(32
 */
