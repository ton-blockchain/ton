struct Container<T> {
    item: T;
}

fun Container<T?>.createFrom<U>(item: U): Container<T?> {
    return { item };
}

fun cantUseAsNonCall() {
    Container<int?>.createFrom<slice>;              // ok
    Container<slice>.createFrom<Container<int>>;    // ok

    Container<int?>.createFrom;
}

/**
@compilation_should_fail
@stderr can not use a generic function `Container<T?>.createFrom<U>` as non-call
@stderr Container<int?>.createFrom;
 */
