fun setCoins(mutate c: coins) {
    c = 0;
}

fun main() {
    var i = 0;
    setCoins(mutate i);
}

/**
@compilation_should_fail
@stderr can not pass `int` to mutate `coins`, because mutation is not type compatible
 */
