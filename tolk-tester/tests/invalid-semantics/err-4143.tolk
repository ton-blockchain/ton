type A<T = A<int>> = int

/**
@compilation_should_fail
@stderr type `A` circularly references itself
 */
