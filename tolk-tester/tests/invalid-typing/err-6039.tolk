fun main() {
    var t = createEmptyTuple();
    t.0 = (1, 2);
}

/**
@compilation_should_fail
@stderr a tuple can not have `(int, int)` inside, because it occupies 2 stack slots in TVM, not 1
*/
