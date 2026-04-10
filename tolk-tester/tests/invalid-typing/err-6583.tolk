fun cantAssignIncompatibleUnionTypes(a: int | slice | builder) {
    var b: int | slice = match (a) {
        int => a,
        slice => a,
        builder => null,
    };
}

/**
@compilation_should_fail
@stderr can not assign `int | slice | null` to variable of type `int | slice`
 */
