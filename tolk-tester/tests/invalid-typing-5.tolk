fun incNotChained(mutate self: int) {
    self = self + 1;
}

fun failWhenReturnANotChainedValue(x: int): int {
    return x.incNotChained();
}

/**
The error is very weird, but nevertheless, the type system prevents of doing such errors.

@compilation_should_fail
@stderr previous function return type int cannot be unified with return statement expression type (): cannot unify type () with int
 */
