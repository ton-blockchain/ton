fun main() {
    var i: int;
    match (random.uint256()) {
        0 => { i = 0; debug.print(i) },
        1 => { if (true) { i = 1 } },
        2 => throw i = 2,
    }
    if (i == 0) {}
}

/**
@compilation_should_fail
@stderr using variable `i` before it's definitely assigned
@stderr if (i == 0)
 */
