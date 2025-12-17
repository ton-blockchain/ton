struct Nested {
    n1: int
    n2: int
}

struct Demo {
    a: int
    b: int
    c: int
    n1: Nested
    n2: Nested
}

fun Nested.mut(mutate self, x: int): int {
    return x
}

fun int.del(mutate self): int {
    self = 0;
    return self
}

fun f(d: Demo) {
    d.n1.mut(d.n1!.n2.del())
}

/**
@compilation_should_fail
@stderr can not borrow `d.n1.n2` for mutation, because `d.n1` is already being mutated by `Nested.mut`
@stderr ^^^^^^^
 */
