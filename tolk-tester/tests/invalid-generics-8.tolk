fun withT1T2<T1, T2>(a: (T1, T2)) {}

fun wrongTCountPassed() {
    withT1T2<int>((5, ""));
}

/**
@compilation_should_fail
@stderr wrong count of generic T: expected 2, got 1
@stderr <int>
 */
