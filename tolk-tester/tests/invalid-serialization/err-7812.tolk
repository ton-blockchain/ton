struct(0b000) B {}
struct C {}

struct A {
    multiple: (int32, B | C);
}

fun main() {
    var a: A = { multiple: (5, B{}) };
    return a.toCell();
}

/**
@compilation_should_fail
@stderr error: auto-serialization via toCell() is not available for type `A`
@stderr because field `A.multiple` of type `(int32, B | C)` can't be serialized
@stderr because element `tensor.1` of type `B | C` can't be serialized
@stderr because could not automatically generate serialization prefixes for a union
@stderr because struct `B` has opcode, but `C` does not
 */
