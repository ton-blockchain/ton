fun increment(mutate x: int) {
    x += 1;
}

fun testCantCallWithoutMutate() {
    var x = 0;
    increment(x);
}

/**
@compilation_should_fail
@stderr function `increment` mutates parameter `x`
@stderr specify `mutate` when passing an argument, like `mutate x`
@stderr in function `testCantCallWithoutMutate`
@stderr increment(x)
@stderr note: parameter declared here
@stderr fun increment(mutate x: int)
 */
