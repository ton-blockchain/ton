fun assign0(mutate x: uint8) {
    x = 0;
}

fun testCantPassDifferentIntN(x: int8) {
    assign0(mutate (x as int));             // ok
    assign0(mutate (x as uint8));           // ok
    assign0(mutate ((x as int64) as int));  // ok
    assign0(mutate x);
}

/**
@compilation_should_fail
@stderr can not pass `int8` to `uint8`
@stderr assign0(mutate x);
 */
