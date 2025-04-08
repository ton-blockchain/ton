type MInt = int;

fun failMathOnBoolean(c: cell) {
    var ten: MInt = 10;
    return (null == c) * ten;
}

/**
@compilation_should_fail
@stderr can not apply operator `*` to `bool` and `MInt`
 */
