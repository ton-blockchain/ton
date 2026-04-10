@pure
fun tuple.tupleMut(mutate self): int
    asm "TLEN";

fun main() {
    var t = createEmptyTuple();
    return [[t.tupleMut]];
}

/**
@compilation_should_fail
@stderr saving `tuple.tupleMut` into a variable is impossible, since it has `mutate` parameters
 */
