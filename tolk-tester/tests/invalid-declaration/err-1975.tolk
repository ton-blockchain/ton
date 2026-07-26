type A<T = B> = T;
type B<U = A> = U;

fun main(): A {
    return 0;
}

/**
@compilation_should_fail
@stderr type `A` circularly references itself
 */
