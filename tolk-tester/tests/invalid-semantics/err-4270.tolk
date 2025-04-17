struct A {
    tens: (int, int) = (1, 2 + now()),
}

/**
@compilation_should_fail
@stderr not a constant expression
 */
