fun main() {
    var t = createEmptyTuple();
    t.0 = (1, 2);
}

/**
@compilation_should_fail
@stderr can not put `(int, int)` into a tuple, because it occupies 2 stack slots in TVM, not 1
*/
