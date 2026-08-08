type AssetBuilder = builder
type BalanceBuilder = builder

fun AssetBuilder.validate(self) { return 0 }
fun BalanceBuilder.validate(self) { return 0 }

fun main() {
    (beginCell() as AssetBuilder).validate();       // ok
    (beginCell() as BalanceBuilder).validate();     // ok

    beginCell().validate();
}

/**
@compilation_should_fail
@stderr method `validate` not found for type `builder`
@stderr but it exists for types `AssetBuilder`, `BalanceBuilder`
 */
