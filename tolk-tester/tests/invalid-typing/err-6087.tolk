fun cantAutoCastBytesNToSlice() {
    var b = beginCell().storeInt(1, 32).endCell().beginParse() as bits32;
    return b.loadInt(32);
}

/**
@compilation_should_fail
@stderr can not call method for `slice` with object of type `bits32`
 */
