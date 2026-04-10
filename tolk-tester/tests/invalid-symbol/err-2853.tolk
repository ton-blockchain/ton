fun main(): int {
    var (a: int, b: int) = (10, 20);
    var (a, b: int) = (10, 20);
}

/**
@compilation_should_fail
@stderr redeclaration of local variable `a`
 */
