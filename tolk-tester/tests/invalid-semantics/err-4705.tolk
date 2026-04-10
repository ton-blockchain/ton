fun increment(mutate x: int) {

}

fun main() {
    var x = 0;
    var inc = increment;
    inc(x);
}

/**
@compilation_should_fail
@stderr saving `increment` into a variable is impossible, since it has `mutate` parameters and thus can only be called directly
 */
