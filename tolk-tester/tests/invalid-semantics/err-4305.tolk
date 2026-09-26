fun main(flag: bool): int {
    var x: int;
    do {
        if (flag) {
            continue;
        }
        x = 1;
    } while (false);
    return x;
}

/**
@compilation_should_fail
@stderr using variable `x` before it's definitely assigned
 */
