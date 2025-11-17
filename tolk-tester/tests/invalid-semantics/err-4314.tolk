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

fun Demo.assignB(mutate self, newB: int): Demo {
    self.b = newB;
    return self;
}

fun mutateDemoAndGetC(mutate d: Demo): int {
    return d.c
}

fun m(d1: Demo, d2: Demo) {
    // ok
    d1.assignB(10);
    d1.assignB(mutateDemoAndGetC(mutate d2));
    d1.assignB(d2.a += d1.b);
    d1.assignB(d2.c = d1.n1.n1);

    // err
    d1.assignB(mutateDemoAndGetC(mutate d1));
}

/**
@compilation_should_fail
@stderr can not borrow `d1` for mutation once again, it is already being mutated by `Demo.assignB`
 */
