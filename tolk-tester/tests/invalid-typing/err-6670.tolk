fun f(a: int, i: int) {}

fun main() {
    f(1, ( (((""))) ))
}

/**
Check that a whole argument is underlined (despite the fact that parentheses don't exist in AST):
@compilation_should_fail
@stderr can not pass `string` to `int`
@stderr f(1, ( (((""))) ))
@stderr      ^^^^^^^^^^^^
 */
