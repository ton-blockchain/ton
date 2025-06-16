fun f(a: int, b: int = a) {

}

/**
@compilation_should_fail
@stderr symbol `a` is not a constant
 */
