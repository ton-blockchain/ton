fun f() {
    __expect_inline(true);
    return 5;
}

fun main() {
    f;      // using as non-call prevents inlining
    return f();
}

/**
@compilation_should_fail
@stderr __expect_inline failed
 */
