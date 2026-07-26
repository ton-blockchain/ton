fun main() {
    var x: int;
    if (true || (x = 1)) {}
    return x;
}

/**
@compilation_should_fail
@stderr using variable `x` before it's definitely assigned
 */
