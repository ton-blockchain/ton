fun f(mutate x: int) {

}

fun main() {
    var x = 5 as int|builder;
    f(mutate x);
}

/**
@compilation_should_fail
@stderr can not pass `int | builder` to `int`
 */
