fun main(x: int): int {
    if (x > 0) {
        var y: int = 10;
    } else {
        var y: slice = "20";
    }
    debug.print(y);
}

/**
@compilation_should_fail
@stderr debug.print(y);
@stderr undefined symbol `y`
 */
