struct B<T> { x: A<T> }
type A<T> = B<T> | int

contract Demo {}

fun onInternalMessage(x: A<int>) {}

/**
@compilation_should_fail
@stderr type `A<int>` circularly references itself
 */
