struct A<T = B> {
    value: T
}

struct B<U = A> {
    value: U
}

fun main(a: A) {
    return a;
}

/**
@compilation_should_fail
@stderr type `A` circularly references itself
 */
