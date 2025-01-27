fun proxy(x: int) {
    return factorial(x);
}

fun factorial(x: int) {
    if (x <= 0) {
        return 1;
    }
    return x * proxy(x-1);
}

/**
@compilation_should_fail
@stderr could not infer return type of `factorial`, because it appears in a recursive call chain
@stderr fun factorial
 */
