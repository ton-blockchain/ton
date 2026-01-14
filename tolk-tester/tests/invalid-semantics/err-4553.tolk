
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
@stderr hint: it's declared `val myVal`, keyword 'val' means 'immutable'
@stderr hint: declare `var myVal` to allow modifications
 */
