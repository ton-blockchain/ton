type IntOrSlice = int | slice;

fun unionTypesNotAllowedInMatch(a: int | slice) {
    match (a) {
        IntOrSlice => 1,
    };
}

/**
@compilation_should_fail
@stderr wrong pattern matching: union types are not allowed, use concrete types in `match`
@stderr IntOrSlice
 */
