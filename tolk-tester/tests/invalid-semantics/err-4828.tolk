fun asdf(mutate cs: slice) {}

fun main(cs: slice) {
    cs.asdf();
}

/**
@compilation_should_fail
@stderr function `asdf` mutates parameter `cs`
@stderr consider calling `asdf(mutate cs)`, not `cs.asdf`()
@stderr alternatively, rename parameter to `self` to make it a method
 */
