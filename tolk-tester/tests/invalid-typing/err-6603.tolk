struct A {
    kk: tuple
    dd: [int, int]
}

fun main(a: A) {
    a.kk = a.dd
}

/**
@compilation_should_fail
@stderr can not assign `[int, int]` to field of type `tuple`
@stderr hint: use `as` operator for UNSAFE casting: `<some_expr> as tuple`
 */
