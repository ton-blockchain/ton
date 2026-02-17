struct WithDefaults<T1, T2 = slice> {
}

fun WithDefaults<T1, T2>.methodBoth(self) {}

fun WithDefaults<T1>.method1(self) {}

fun main() {
    var eq = WithDefaults<int>{};
    __expect_type(eq, "WithDefaults<int, slice>");
    eq.methodBoth();
    eq.method1();

    var diff = WithDefaults<int, cell>{};
    __expect_type(diff, "WithDefaults<int, cell>");
    diff.methodBoth();

    diff.method1();     // err
}

/**
@compilation_should_fail
@stderr method `method1` not found for type `WithDefaults<int, cell>`
@stderr (but it exists for type `WithDefaults<T1, slice>`  
 */
