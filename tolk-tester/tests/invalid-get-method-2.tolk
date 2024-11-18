@pure
get fun secret(): int {
    return 0;
}
@pure
get fun balanced(): int {
    return 1;
}

fun main(): int {
    return secret() + balanced();
}

/**
@compilation_should_fail
@stderr GET methods hash collision: `secret` and `balanced` produce the same hash
*/
