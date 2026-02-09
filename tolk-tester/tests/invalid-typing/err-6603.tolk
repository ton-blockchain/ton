struct A {
    kk: tuple
    ss: array<slice>
    dd: [int, int]
}

fun main(a: A) {
    a.kk = a.ss;        // ok
    a.ss = a.dd;        // err
}

/**
@compilation_should_fail
@stderr can not assign `[int, int]` to field of type `array<slice>`
@stderr_avoid ok
 */
