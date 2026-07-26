struct A {
    x: int = B{}.y
}

struct B {
    y: int = A{}.x
}

fun main() {
    return 0;
}

/**
@compilation_should_fail
@stderr field `y` default value circularly references itself
 */
