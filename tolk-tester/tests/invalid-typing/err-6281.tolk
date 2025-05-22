struct Wrapper<T> { item: T; }

fun Wrapper<T>.create(initial: T): Wrapper<T> { return { item: initial }; }

fun main() {
    Wrapper<Wrapper<builder>>.create(123);
}

/**
@compilation_should_fail
@stderr can not pass `int` to `Wrapper<builder>`
 */
