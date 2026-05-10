fun f(x: int32 | void | int64) {
    x.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `int32 | void | int64`
@stderr because could not automatically generate serialization prefixes for a union
@stderr because `void` must be the last variant of a union
 */
