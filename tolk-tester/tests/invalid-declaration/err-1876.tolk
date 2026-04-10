struct Pair<T1, T2> {
    first: T1;
    second: T2;
    first: int;
}

/**
@compilation_should_fail
@stderr redeclaration of field `first`
@stderr first: int
 */
