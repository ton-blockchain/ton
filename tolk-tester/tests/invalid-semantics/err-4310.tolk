fun main(): int {
    var x: int;
    (fun() {
        x;
    })();
    return 0;
}

/**
@compilation_should_fail
@stderr using variable `x` before it's definitely assigned
 */
