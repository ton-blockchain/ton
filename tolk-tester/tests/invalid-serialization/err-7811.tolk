struct(0b01) B {}
struct(0b00) C {}

struct A {
    multiple: B | C | null;
}

fun main() {
    var a: A = { multiple: B{} };
    return a.toCell();
}

/**
@compilation_should_fail
@stderr error: auto-serialization via toCell() is not available for type `A`
@stderr because field `A.multiple` of type `B | C | null` can't be serialized
@stderr because could not automatically generate serialization prefixes for a union
@stderr because of mixing primitives and struct `C` with serialization prefix
 */
