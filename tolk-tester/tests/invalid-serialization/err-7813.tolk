struct (0x0F) A {}

fun f(x: int32 | A) {
    x.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `int32 | A`
@stderr because could not automatically generate serialization prefixes for a union
@stderr because of mixing primitives and struct `A` with serialization prefix
@stderr hint: extract primitives to single-field structs and provide prefixes
 */
