fun takeInvalidTuple(t: [int, (int, builder), int]) {
}

fun main() {
    takeInvalidTuple([1, (2, beginCell()), 0]);
}

/**
@compilation_should_fail
@stderr can not put `(int, builder)` into a tuple, because it occupies 2 stack slots in TVM, not 1
 */
