@inline_ref
fun asmInlineRef(): int asm "1 PUSHINT"

/**
@compilation_should_fail
@stderr inline annotations are not applicable to asm functions
 */
