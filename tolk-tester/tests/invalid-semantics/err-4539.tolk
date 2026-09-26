import "@stdlib/tvm-dicts"

type BalanceList = dict
type AssetList = dict

fun BalanceList.validate(self): int1 { return 0 }
fun AssetList.validate(self): int2 { return 0 }

fun f(b: cell) {
    b.validate()
}

/**
@compilation_should_fail
@stderr method `validate` not found for type `cell`
@stderr but it exists for types `BalanceList`, `AssetList`
 */
