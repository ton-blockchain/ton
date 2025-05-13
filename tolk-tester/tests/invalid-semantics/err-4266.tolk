fun invalidReferencingGenericMethodWithoutGeneric() {
    var t = createEmptyTuple();
    var cb = t.last;
}

/**
@compilation_should_fail
@stderr can not use a generic function `tuple.last<T>` as non-call
 */
