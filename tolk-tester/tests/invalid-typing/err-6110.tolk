fun makeNullable<T1, T2>(a: T1 | T2): T1 | T2 | null {
    return a;
}

fun cantDeduceDefiniteUnion(a: int | slice | builder) {
    makeNullable<int | slice, builder>(a);      // ok
    makeNullable<int, slice | builder>(a);      // ok
    makeNullable(a);
}

/**
@compilation_should_fail
@stderr can not deduce T1 for generic function `makeNullable<T1, T2>`
@stderr makeNullable(a);
 */
