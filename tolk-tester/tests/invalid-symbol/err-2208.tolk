fun main(x: int | slice) {
    match (var a = x) {     // a variable declared in `match` subject exists only within a match
        int => a + 0,
        slice => a.loadInt(32),
    }
    return a;
}

/**
@compilation_should_fail
@stderr undefined symbol `a`
@stderr return a;
 */
