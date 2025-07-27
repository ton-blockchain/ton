fun analyze(t: slice) {}

fun main(t: slice?) {
    analyze(t);
}

/**
@compilation_should_fail
@stderr can not pass `slice?` to `slice`
@stderr hint: probably, you should check on null
@stderr hint: alternatively, use `!` operator to bypass nullability checks: `<some_expr>!`
 */
