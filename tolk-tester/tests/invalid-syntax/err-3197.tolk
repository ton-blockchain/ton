fun load_u32(cs: slice): (slice, int) {
    return cs.load_uint 32;
}

/**
@compilation_should_fail
@stderr expected `;`, got `32`
 */
