const ONE = TWO - 1;
const TWO = ONE + 1;

/**
@compilation_should_fail
@stderr const `TWO` appears, directly or indirectly, in its own initializer
 */
