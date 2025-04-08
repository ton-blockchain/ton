@pure
fun tupleMut(mutate self: tuple): int
    asm "TLEN";

fun main() {
    var t = createEmptyTuple();
    return [[t.tupleMut]];
}

/**
@compilation_should_fail
@stderr saving `tupleMut` into a variable is impossible, since it has `mutate` parameters
 */
