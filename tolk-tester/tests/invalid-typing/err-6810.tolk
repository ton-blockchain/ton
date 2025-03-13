fun cantCastBoolToCoins() {
    var k1 = true as int8;              // ok
    var k2 = true as int1;              // ok
    var k3 = (true as int) as coins;    // ok
    var k4 = true as coins;
}

/**
@compilation_should_fail
@stderr type `bool` can not be cast to `coins`
@stderr true as coins
 */
