struct A {
    ok: coins = ton("0.05"),
    err: int = now(),
}

/**
@compilation_should_fail
@stderr not a constant expression
@stderr err: int
 */
