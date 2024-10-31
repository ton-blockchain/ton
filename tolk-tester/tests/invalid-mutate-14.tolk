fun main(cs: slice) {
    return loadInt(cs, 32);
}

/**
@compilation_should_fail
@stderr `loadInt` is a mutating method; consider calling `cs.loadInt()`, not `loadInt(cs)`
 */
