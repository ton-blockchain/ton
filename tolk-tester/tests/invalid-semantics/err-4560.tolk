struct Wrapper<T> { item: T }

type AssetBuilder = builder
type AssetWrapper = Wrapper<AssetBuilder>

fun AssetWrapper.validate(self) { return 0 }

fun main() {
    var b: Wrapper<AssetBuilder> = Wrapper{item: beginCell()};
    b.validate();
}

/**
@compilation_should_fail
@stderr method `validate` not found for type `Wrapper<AssetBuilder>`
@stderr but it exists for type `AssetWrapper`
 */
