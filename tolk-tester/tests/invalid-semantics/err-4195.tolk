fun getNullableTuple(): tuple? { return createEmptyTuple(); }

@pure
fun tuplePush<T>(mutate t: tuple, value: T): void
    asm "TPUSH";

fun cantUseLValueUnwrappedNotNull() {
    tuplePush(mutate getNullableTuple()!, 1);
}

/**
@compilation_should_fail
@stderr function call can not be used as lvalue
 */
