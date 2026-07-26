type AliasToInt = int

struct Wrapper<T> {
    item: T | int
}

fun main(w: Wrapper<AliasToInt>) {}

/**
@compilation_should_fail
@stderr union variants `AliasToInt` and `int` have the same runtime representation
 */
