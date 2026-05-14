struct(0x12345678) A {
    x: uint8
}

struct(0x12345678) B {
    y: uint16
}

type U = A | B

fun main() {
    U.fromCell(beginCell().endCell());
}

/**
@compilation_should_fail
@stderr auto-serialization via fromCell() is not available for type `U`
@stderr because alias `U` expands to `A | B`
@stderr because both structs `A` and `B` have serialization prefix 0x12345678
 */

