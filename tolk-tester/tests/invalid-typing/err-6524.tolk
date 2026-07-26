struct Wrapper<T> {
    item: T
}

fun main(x: Wrapper<int | builder>) {
    x as Wrapper<int | builder> | Wrapper<builder | int>;
}

/**
@compilation_should_fail
@stderr union variants `Wrapper<int | builder>` and `Wrapper<builder | int>` have the same runtime representation
 */
