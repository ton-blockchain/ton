fun main(x: int): int {
    var (a: int, b: int) = (10, 20);
    var (a redef, x: int) = (10, 20);
}

/**
@compilation_should_fail
@stderr redeclaration of local variable `x`
 */
