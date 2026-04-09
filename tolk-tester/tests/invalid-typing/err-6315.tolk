fun int.methodForInt(self) {}

fun mutableInt(mutate i: int) {}

fun main(b: builder) {
    // after `f(mutate expr)`, expr's type becomes param_type of f
    mutableInt(mutate b);
    // that's why this method is found
    b.methodForInt();

    // but after inferring, type checker fails
}

/**
@compilation_should_fail
@stderr can not pass `builder` to `int`
@stderr mutableInt(mutate b)
 */
