get fun asmNotAllowed(): int asm "NOP";

/**
@compilation_should_fail
@stderr invalid declaration of a get method
@stderr asmNotAllowed
@stderr ^^^^^^^^^^^^^
 */
