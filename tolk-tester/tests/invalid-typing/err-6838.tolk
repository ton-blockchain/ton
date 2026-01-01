fun main() {
    array<(int, int)> [(5, 6), null];
}

/**
@compilation_should_fail
@stderr type `null` is not assignable to `(int, int)`
 */
