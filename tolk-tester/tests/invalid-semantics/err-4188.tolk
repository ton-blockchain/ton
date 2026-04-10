fun recursive1(): int {
    return recursive2();
}

fun recursive2(): int {
    __expect_inline(true);
    return recursive1();
}

/**
@compilation_should_fail
@stderr __expect_inline failed
 */
