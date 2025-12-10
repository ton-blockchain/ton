fun int.add(mutate self, dx: int) {
    self += dx
}

fun int.del(mutate self): int {
    self = 0;
    return self
}

fun main() {
    var x = 10;
    x.add(x.del());
}

/**
@compilation_should_fail
@stderr can not borrow `x` for mutation once again, it is already being mutated by `int.add`
@stderr hint: split a complex expression into several simple ones
 */
