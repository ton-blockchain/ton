struct Wrapper<T> { item: T }

type AssetBuilder = builder
type BalanceBuilder = builder

fun Wrapper<AssetBuilder>.validate(self) { return 0 }

fun main() {
    var b: BalanceBuilder = beginCell();
    Wrapper{item:b}.validate();
}

/**
@compilation_should_fail
@stderr method `validate` not found for type `Wrapper<BalanceBuilder>`
@stderr but it exists for type `Wrapper<AssetBuilder>`
 */
