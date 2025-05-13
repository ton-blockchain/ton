struct A {
    a: int = 1 << CCC;
}

/**
@compilation_should_fail
@stderr undefined symbol `CCC`
@stderr 1 << CCC
 */
