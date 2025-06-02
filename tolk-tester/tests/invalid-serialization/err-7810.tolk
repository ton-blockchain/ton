struct(0b01) B {}
struct(0b00) C {}

struct A {
    multiple: B | C | int32;
}

fun main() {
    var a: A = { multiple: B{} };
    return a.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `A`
@stderr because field `A.multiple` of type `B | C | int32` can't be serialized
@stderr because could not automatically generate serialization prefixes for a union
@stderr because of mixing primitives and struct `C` with serialization prefix
@stderr hint: extract primitives to single-field structs and provide prefixes
 */
