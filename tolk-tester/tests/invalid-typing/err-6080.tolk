fun incrementOrSetNull(mutate x: int?) {
    if (random.uint256()) { x! += 1; }
    else { x = null; }
}

fun cantCallMutateMethodNotNullable() {
    var x = 1;
    incrementOrSetNull(mutate x);
    return x;
}

/**
@compilation_should_fail
@stderr can not pass `int` to mutate `int?`
@stderr because mutation is not type compatible
 */
