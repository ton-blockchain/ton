type AliasToInt = int

struct Wrapper<T> {
    item: T
}

fun main(x: Wrapper<AliasToInt>) {
    x is Wrapper<AliasToInt> | Wrapper<int>;
}

/**
@compilation_should_fail
@stderr union variants `Wrapper<AliasToInt>` and `Wrapper<int>` have the same runtime representation
 */
