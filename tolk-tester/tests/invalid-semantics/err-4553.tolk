
fun inc(mutate x: int) {
    x += 1;
}

fun cantPassToMutatingFunction() {
    val myVal = 10;
    inc(mutate myVal);
}

/**
@compilation_should_fail
@stderr modifying immutable variable `myVal`
 */
