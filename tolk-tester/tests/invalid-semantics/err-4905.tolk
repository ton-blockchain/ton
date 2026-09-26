struct Wrapper<T> {
    item: T
}

type AssetBuilder = builder
type BalanceBuilder = builder

fun Wrapper<AssetBuilder>.validate(self): int3 { return 0 }
fun Wrapper<BalanceBuilder>.validate(self): int4 { return 0 }

fun main() {
    var a: builder = beginCell();
    return Wrapper{item: a}.validate();
}

/**
@compilation_should_fail
@stderr method `validate` not found for type `Wrapper<builder>`
@stderr but it exists for types `Wrapper<AssetBuilder>`, `Wrapper<BalanceBuilder>`
 */
