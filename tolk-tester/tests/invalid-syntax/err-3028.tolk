fun noOpReturnsInt(): int asm
    "NOP"
    ""

/**
@compilation_should_fail
@stderr invalid asm instruction
@stderr ""
 */
