global hello: int;

fun hello(): int {

}

/**
@compilation_should_fail
@stderr fun hello()
@stderr redefinition of symbol `hello`
@stderr previous definition is here
@stderr err-1918.tolk:1
 */
