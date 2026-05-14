fun main() {
    do {
        var x = 1;
    } while (x);
}

/**
@compilation_should_fail
@stderr undefined symbol `x`
 */
