fun ok(): (int, slice) asm(->0b1 0b0) "NOP"
fun not(): (int, slice) asm(->0xFFFFF) "NOP"

/**
@compilation_should_fail
@stderr ret_order contains invalid integer, not in range 0 .. N
@stderr fun not
 */
