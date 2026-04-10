struct Container<T> {
    item: T;
}

fun Container<T>.wrap(item: T) {}

fun main(c: Container<int>) {
    c.wrap();
}

/**
@compilation_should_fail
@stderr method `Container<int>.wrap` can not be called via dot
@stderr it's a static method, it does not accept `self`
@stderr c.wrap();
 */
