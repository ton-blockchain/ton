fun matchOverNotSubtypeIsNever(m: slice | cell) {
    match (m) {
        int => return m + 0
    };
    return 3;
}

/**
@compilation_should_fail
@stderr `int` is not a variant of `slice | cell`
@stderr int =>
 */
