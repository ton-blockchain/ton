fun eq<X>(t: X) { return t; }

fun failUsingGenericFunctionPartially() {
    var cb = createEmptyTuple().eq().eq().tuplePush;
}

/**
@compilation_should_fail
@stderr can not use a generic function `tuplePush<T>` as non-call
 */
