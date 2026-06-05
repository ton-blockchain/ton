struct Leaf {
    s: string
    i: int8
}

struct Outer5 {
    a: Leaf
    b: Leaf
    c: Leaf
    d: Leaf
    e: Leaf
}

fun main(c: Outer5): int {
    return c.toCell();
}

/**
@compilation_should_fail
@stderr struct `Outer5` can exceed 4 refs in serialization (estimated size: 5..5 refs)
@stderr can not convert type `Cell<Outer5>` to return type `int`
 */
