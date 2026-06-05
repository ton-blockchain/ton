const SHIFT: int = 1 << 32;
const TOO_BIG: int = 1 << SHIFT;

/**
@compilation_should_fail
@stderr integer overflow
 */
