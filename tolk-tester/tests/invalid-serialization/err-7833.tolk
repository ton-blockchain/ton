struct (0x01) A {}
struct (0x02) B {}

type U = A | B
type AliasU = U

fun AliasU.packToBuilder(self, mutate b: builder) {
    b.storeUint(0, 1);
}

fun AliasU.unpackFromSlice(mutate s: slice): AliasU {
    s.loadUint(1);
    return A {};
}

fun main(s: slice): int {
    val u: AliasU = lazy U.fromSlice(s);
    return match (u) {
        A => 1,
        B => 2,
    };
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `u` is declared as `AliasU`, but lazy expression has type `U`
 */
