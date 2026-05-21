struct A<T> { v: A<A<T>> }
fun use(a: A<int>) {}

/**
@compilation_should_fail
@stderr instantiation of `A<T>` exceeded depth limit; it's most likely caused by an infinitely growing type
 */
