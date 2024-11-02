fun incNotChained(mutate self: int) {
    self = self + 1;
}

fun cantCallNotChainedMethodsInAChain(x: int) {
    return x.incNotChained().incNotChained();
}

/**
The error is very weird, but nevertheless, the type system prevents of doing such errors.

@compilation_should_fail
@stderr cannot apply function incNotChained : int -> (int, ()) to arguments of type (): cannot unify type () with int
 */
