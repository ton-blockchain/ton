global hello: int;

fun hello(): int {

}

/**
@compilation_should_fail
@stderr fun hello()
@stderr redefinition of symbol, previous was at
@stderr err-1918.tolk:1
 */
