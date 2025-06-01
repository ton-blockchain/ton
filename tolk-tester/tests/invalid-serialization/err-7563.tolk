struct A {
    d1: bits256;
    d2: bits256;
    d3: bits256;
    d4: bits256;
    d5: bits256;
}

struct B {
    a: Cell<A>;
}

fun main(b: B) {
    b.toCell();
}

/**
@compilation_should_fail
@stderr struct `A` can exceed 1023 bits in serialization (estimated size: 1280..1280 bits)
 */
