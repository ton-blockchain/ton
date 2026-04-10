fun assign0(mutate x: uint8) {
    x = 0;
}

fun testCantPassDifferentIntN(x: int8) {
    // all of them are incorrent, some due to `as` lvalue, the last due to type incompatibility
    assign0(mutate (x as int));
    assign0(mutate (x as uint8));
    assign0(mutate ((x as int64) as int));
    assign0(mutate x);
}

/**
@compilation_should_fail
@stderr can not pass `int` to mutate `uint8`, because mutation is not type compatible
@stderr can not pass `int8` to mutate `uint8`, because mutation is not type compatible
@stderr can not mutate a temporary expression
 */
