fun getAnySlice() { return beginCell().endCell().beginParse(); }

fun cantAssignSliceToBytesN() {
    var c1: bytes16 = getAnySlice() as bytes16;    // ok
    var c2: bytes16 = getAnySlice();
}

/**
@compilation_should_fail
@stderr can not assign `slice` to variable of type `bytes16`
@stderr var c2: bytes16 = getAnySlice();
 */
