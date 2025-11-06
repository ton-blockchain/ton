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
@stderr call to method `validate` for type `builder` is ambiguous
@stderr candidate function: `AssetBuilder.validate`
@stderr candidate function: `BalanceBuilder.validate`
 */
