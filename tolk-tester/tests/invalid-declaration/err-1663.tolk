struct Container<T> {
    item: T;
}

fun Container<T>.wrap<T>(item: T) {}

/**
@compilation_should_fail
@stderr duplicate generic parameter `T`
@stderr fun Container<T>.wrap<T>
 */
