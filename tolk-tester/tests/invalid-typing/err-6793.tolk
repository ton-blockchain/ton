fun main(cond: bool): int {
    var x: int? = null;
    cond || (x = 1);
    return x + 1;
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `int?` and `int`
 */
