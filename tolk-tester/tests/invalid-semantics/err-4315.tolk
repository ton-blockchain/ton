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

fun Nested.assign(mutate self, n: Nested) {
    self = n
}

fun m(d: Demo) {
    // ok
    d.a += (d.b += (d.c = 10));
    // ok
    d.n1.assign({
        n1: d.n1.n1,
        n2: d.n1.n2,
    });
    // err
    d.n1.assign({
        n1: ((_, d.n1.n2) = (0, 1)).0,
        n2: 10,
    });
}

/**
@compilation_should_fail
@stderr can not borrow `d.n1.n2` for mutation, because `d.n1` is already being mutated by `Nested.assign`
 */
