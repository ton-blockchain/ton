fun coins.set(mutate self) {
    self = 0;
}

fun main() {
    var i = 0;
    i.set();
}

/**
@compilation_should_fail
@stderr can not call method for mutate `coins` with object of type `int`, because mutation is not type compatible
 */
