struct A {
    nan: int = 1 / 0
}

/**
@compilation_should_fail
@stderr integer overflow
@stderr 1 / 0
@stderr ^^^^^
 */
