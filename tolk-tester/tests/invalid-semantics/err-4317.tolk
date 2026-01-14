fun mutateOverlapping(mutate x: int, mutate p: (int, int)): void {
    x += 1;
    p = (3, 4);
}

fun main(): (int, int) {
    var a = 10;
    var b = 20;
    mutateOverlapping(mutate a, mutate (((a, b))));
    return (a, b);
}

/**
@compilation_should_fail
@stderr can not borrow `a` for mutation once again, it is already being mutated by `mutateOverlapping`
 */
