
fun main() {
    createEmptyMap<slice, int8>();
}

/**
@compilation_should_fail
@stderr invalid `map`: type `slice` can not be used as a key
@stderr because it does not specify keyLen for a dictionary
@stderr hint: use `address` if a key is an internal address
@stderr hint: use `bits128` and similar if a key represents fixed-width data
 */
