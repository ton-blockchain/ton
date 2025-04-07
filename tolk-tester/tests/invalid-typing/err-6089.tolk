fun unionTypesNotAllowedInIs(a: int | slice | builder) {
    if (a is int | slice) {}
}

/**
@compilation_should_fail
@stderr union types are not allowed, use concrete types in `is`
 */
