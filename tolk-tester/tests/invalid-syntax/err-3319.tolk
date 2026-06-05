fun t(): (int, int) asm (-> 1222222222222222222222222222222223 0) "NOP"

/**
@compilation_should_fail
@stderr ret_order contains invalid integer, not in range 0 .. N
 */
