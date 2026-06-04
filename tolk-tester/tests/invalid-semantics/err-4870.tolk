fun void.crash(self): int {
    return 1;
}

fun main(): int {
    return void.crash();
}

/**
@compilation_should_fail
@stderr too few arguments in call to `void.crash`, expected 1, have 0
@stderr return void.crash();
 */
