fun calcSum<X>(x: X, y: X) { return x + y; }

fun cantApplyPlusOnNullable() {
    return calcSum(((0 as int?)), null);
}

/**
@compilation_should_fail
@stderr in function `calcSum<int?>`
@stderr can not apply operator `+` to `int?` and `int?`
 */
