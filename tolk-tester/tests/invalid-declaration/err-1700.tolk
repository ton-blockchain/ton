enum Over {
    MaxIntMinus1 = +115792089237316195423570985008687907853269984665640564039457584007913129639935 - 1,
    MaxInt,
    MaxIntPlus1,
}

/**
@compilation_should_fail
@stderr integer overflow
@stderr MaxIntPlus1
 */
