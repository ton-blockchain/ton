fun getAnySlice() { return beginCell().endCell().beginParse(); }

fun cantAssignSliceToBytesN() {
    var c1: bits128 = getAnySlice() as bits128;    // ok
    var c2: bits128 = getAnySlice();
}

/**
@compilation_should_fail
@stderr can not assign `slice` to variable of type `bits128`
@stderr var c2: bits128 = getAnySlice();
@stderr hint: use `as` operator for UNSAFE casting: `<some_expr> as bits128`
 */
