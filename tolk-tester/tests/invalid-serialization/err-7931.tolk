struct (0x01) A {
    x: uint8
}

struct (0x02) B {
    y: uint8
}

fun A.packToBuilder(self, mutate b: builder) {
    b.storeUint(self.x, 8);
}

fun A.unpackFromSlice(mutate s: slice): A {
    return A {x: s.loadUint(8)};
}

type U = A | B

fun f(u: U) {
    u.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `U`
@stderr because alias `U` expands to `A | B`
@stderr because could not automatically generate serialization prefixes for a union
 */
