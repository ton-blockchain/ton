fun cantAutoCastBytesNToSlice() {
    var b = beginCell().storeInt(1, 32).endCell().beginParse() as bits32;
    return b.loadInt(32);
}

/**
@compilation_should_fail
@stderr method `loadInt` not found for type `bits32`
 */
