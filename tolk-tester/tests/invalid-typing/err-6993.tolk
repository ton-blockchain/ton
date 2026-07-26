struct A {
    x: int
}

struct B {
    x: int
    y: int
}

const C: B = A { x: 1 }
const Y = C.y

fun main(): int {
    return Y
}

/**
@compilation_should_fail
@stderr can not assign `A` to `B`
 */
