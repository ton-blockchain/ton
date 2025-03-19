fun add1(x: int) {
    return x + 1;
}

fun main() {
    val adder_fn = add1;
    var x = 10;
    return adder_fn(mutate x);
}

/**
@compilation_should_fail
@stderr `mutate` used for non-mutate parameter
 */
