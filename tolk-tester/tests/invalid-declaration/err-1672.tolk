const BAD_SHIFT: (int, int8) = (0, 7 + (1 << -1));

/**
@compilation_should_fail
@stderr integer overflow
 */
