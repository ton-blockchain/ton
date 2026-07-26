
fun [int8, int8].unpackFromSlice(mutate s: slice): [int8, int8] {
    return [0, 0]
}

/**
@compilation_should_fail
@stderr this method can not be declared for type `[int8, int8]`
@stderr hint: custom pack/unpack can be declared only for type aliases and structures
 */
