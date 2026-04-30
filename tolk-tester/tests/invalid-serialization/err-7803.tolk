struct (0x01) A {
    value: int8
}

struct (0x02) B {
    value: int8
}

type Msg = A | B

fun main(s: slice): int {
    var msg = lazy Msg.fromSlice(s);
    return match (msg) {
        A => 0,
        B => 1,
        else => (msg = A { value: 7 }).value,
    };
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `msg` is used in a non-lazy manner
 */
