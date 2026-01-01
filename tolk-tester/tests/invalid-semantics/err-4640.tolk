fun int.assign10(mutate self) {
    self = 10;
}

fun main() {
    var (i, j) = (0,0);
    (i as int).assign10();
    return (i, j);
}

/**
@compilation_should_fail
@stderr operator `as` can not be used as lvalue
 */
