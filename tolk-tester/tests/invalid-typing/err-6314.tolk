fun int.methodForInt(self) {}
fun cell.methodForCell(self) {}

fun mutableInt(mutate i: int) {}
fun mutableCell(mutate c: cell) {}

fun main(b: builder) {
    // after `f(mutate expr)`, expr's type becomes param_type of f
    mutableInt(mutate b);
    __expect_type(b, "int");
    b.methodForInt();

    if (random.uint256()) {
        mutableCell(mutate b);
        __expect_type(b, "cell");
        b.methodForCell();
    }

    // that's why `endCell()` is not found
    b.endCell();
}

/**
@compilation_should_fail
@stderr method `endCell` not found for type `cell | int`
 */
