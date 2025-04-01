fun testCantCastBoolToUIntN(x: int) {
    var eq = x == 0;
    eq as int8;       // ok
    eq as uint8;
}

/**
@compilation_should_fail
@stderr type `bool` can not be cast to `uint8`
 */
