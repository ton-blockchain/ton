fun justTrue(): int { return true; }

const a = justTrue() | (1 < 9) | justTrue() != true;

/**
@compilation_should_fail
@stderr | has lower precedence than !=
*/
