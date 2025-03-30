fun invalidReferencingGenericMethodWithoutGeneric() {
    var t = createEmptyTuple();
    var cb = t.tupleLast;
}

/**
@compilation_should_fail
@stderr can not use a generic function `tupleLast<T>` as non-call
 */
