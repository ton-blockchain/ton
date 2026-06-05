fun main() {
    var s32 = "00000000".hexToSlice();
    (s32 as slice, 0).0.skipBits(8);
}

/**
@compilation_should_fail
@stderr can not mutate a temporary expression
 */
