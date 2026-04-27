enum Bad1: uint8 {
    Small1 = -1,
    Okay1_v1 = 0,
    Okay1_v2 = 255,
    Big1 = 256,
}

enum Bad2: int2 {
    zero,
    one,
    two,
    three,
}

/**
@compilation_should_fail
@stderr member `Small1` = -1 does not fit into `uint8`
@stderr member `Big1` = 256 does not fit into `uint8`
@stderr member `two` = 2 does not fit into `int2`
@stderr member `three` = 3 does not fit into `int2`
@stderr_avoid zero
@stderr_avoid Okay1
 */
