import "@stdlib/tvm-dicts"

type BalanceList = dict
type AssetList = dict

fun BalanceList.validate(self): int1 { return 0 }
fun AssetList.validate(self): int2 { return 0 }

fun f(b: BalanceList) {
    if (b == null) { return }
    else { b = createEmptyDict() }
    __expect_type(b, "BalanceList");

    if (b == null) { return }
    __expect_type(b, "cell");

    b.validate()
}

/**
@compilation_should_fail
@stderr call to method `validate` for type `cell` is ambiguous
@stderr candidate function: `BalanceList.validate`
@stderr candidate function: `AssetList.validate`
 */
