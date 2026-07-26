struct (0x01) A {}
struct (0x02) B {}

type AB = A | B
type Wider = A | B | int

fun main(s: slice): int {
    val msg: Wider = lazy AB.fromSlice(s);
    return match (msg) {
        A => 1,
        B => 2,
        int => 0,
    };
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `msg` is declared as `Wider`, but lazy expression has type `AB`
 */
