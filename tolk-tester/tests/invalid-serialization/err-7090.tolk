struct S {
    x: int8
}

type AliasS = S;

fun AliasS.packToBuilder(self, mutate b: builder): void {
    b.storeInt(self.x, 8);
}

fun AliasS.unpackFromSlice(mutate s: slice): AliasS {
    return (S{x: s.loadInt(8)} as AliasS);
}


fun S.unpackFromSlice(mutate s: slice): S {
    return S{x: s.loadInt(16)};
}


fun main() {
    val v: AliasS = (S{x: 7} as AliasS);
    return v.toCell();
}

/**
@compilation_should_fail
@stderr ambiguous method, both `AliasS.unpackFromSlice` and `S.unpackFromSlice` are applicable
 */
