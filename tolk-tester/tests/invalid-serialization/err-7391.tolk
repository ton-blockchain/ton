struct (0x01) A {
    v: int8
}

fun f(x: void | A) {
    x.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `void | A`
@stderr because could not automatically generate serialization prefixes for a union
@stderr because `void` must be the last variant of a union
 */
