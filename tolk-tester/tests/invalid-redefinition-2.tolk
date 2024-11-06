global hello: int;

fun hello(): int {

}

/**
@compilation_should_fail
@stderr fun hello()
@stderr redefinition of symbol, previous was at
@stderr invalid-redefinition-2.tolk:1:1
 */
