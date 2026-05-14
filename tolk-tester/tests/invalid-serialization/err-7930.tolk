struct (0x01) A { x: int32 }
struct B { y: int64 }

contract BadDirective {
    storage: A | B
}

fun main() {
    return 0;
}

/**
@compilation_should_fail
@stderr `storage` can not be serialized: type `A | B`
@stderr because could not automatically generate serialization prefixes for a union
@stderr because struct `A` has opcode, but `B` does not
 */
