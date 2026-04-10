fun f() {
    var n: int<slice> = 10;
}

/**
@compilation_should_fail
@stderr type `int` is not generic
 */
