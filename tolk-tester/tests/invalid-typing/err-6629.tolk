fun withT1T2<T1, T2>(a: (T1, T2)) {}

fun wrongTCountPassed() {
    withT1T2<int>((5, ""));
}

/**
@compilation_should_fail
@stderr expected 2 type arguments, got 1
@stderr <int>
 */
