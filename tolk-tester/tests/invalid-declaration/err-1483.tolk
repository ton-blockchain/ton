type A = B;
type B = (int, A);

/**
@compilation_should_fail
@stderr type `A` circularly references itself
@stderr type A = B
 */
