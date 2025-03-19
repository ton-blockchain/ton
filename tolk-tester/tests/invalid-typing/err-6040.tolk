fun main() {
    var t = createEmptyTuple();
    var xy = t.0 as (int, int);
}

/**
@compilation_should_fail
@stderr a tuple can not have `(int, int)` inside, because it occupies 2 stack slots in TVM, not 1
*/
