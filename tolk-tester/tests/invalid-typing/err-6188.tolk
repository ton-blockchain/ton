fun failMathOnBoolean(c: cell) {
    return (null == c) * 10;
}

/**
@compilation_should_fail
@stderr can not apply operator `*` to `bool` and `int`
 */
