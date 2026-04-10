// this function is declared incorrectly,
// since it should return 2 values onto a stack (1 for returned slice, 1 for mutated int)
// but contains not 2 numbers in asm ret_order
fun int.loadAddress2(mutate self): slice
    asm( -> 1 0 2) "LDMSGADDR";

fun main(){}

/**
@compilation_should_fail
@stderr ret_order (after ->) expected to contain 2 numbers
@stderr asm( -> 1 0 2)
 */
