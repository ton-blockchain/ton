fun int.incSelfWithOther(mutate self, mutate other: int) {
    self += 1;
    other += 1;
}

fun m() {
    var x = 0;
    var y = 0;
    x.incSelfWithOther(mutate y);       // ok
    x.incSelfWithOther(mutate x);
}

/**
@compilation_should_fail
@stderr can not borrow `x` for mutation once again, it is already being mutated by `int.incSelfWithOther`
 */
