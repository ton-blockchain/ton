fun getNullableTuple(): tuple? { return createEmptyTuple(); }

fun cantUseLValueUnwrappedNotNull() {
    tuplePush(mutate getNullableTuple()!, 1);
}

/**
@compilation_should_fail
@stderr function call can not be used as lvalue
 */
