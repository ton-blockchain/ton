fun main() {
    array<(int, int)> [(5, 6), null];
}

/**
@compilation_should_fail
@stderr can not convert `null` to `(int, int)`
 */
