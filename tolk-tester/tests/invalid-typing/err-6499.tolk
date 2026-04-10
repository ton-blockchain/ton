fun testLogicalAndNotConditionDoesntAffect(x: int?) {
    var gt1 = x != null && x > 1;
    return x + 0;
}

/**
@compilation_should_fail
@stderr can not apply operator `+` to `int?` and `int`
 */
