fun main(x: int): int {
    if (x > 0) {
        var y: int = 10;
    } else {
        var y: slice = "20";
    }
    debugPrint(y);
}

/**
@compilation_should_fail
@stderr debugPrint(y);
@stderr undefined symbol `y`
 */
