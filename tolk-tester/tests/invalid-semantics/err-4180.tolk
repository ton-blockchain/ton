fun main() {
    var f = fun(mutate x: int) { x = 10 };
}

/**
@compilation_should_fail
@stderr `mutate` is not available in lambdas
@stderr  ^^^^^^ 
 */
