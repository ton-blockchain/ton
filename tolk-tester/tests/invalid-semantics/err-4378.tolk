struct Wrapper<T> {
    field: T;
}

fun f(w: Wrapper<Wrapper>) {
}

/**
@compilation_should_fail
@type `Wrapper<T>` is generic, you should provide type arguments
 */
