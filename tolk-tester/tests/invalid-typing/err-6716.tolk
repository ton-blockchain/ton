fun f(d: bits8 = "") {}

/**
@compilation_should_fail
@stderr can not assign `slice` to `bits8`
@stderr hint: use `as` operator for UNSAFE casting: `<some_expr> as bits8`
 */
