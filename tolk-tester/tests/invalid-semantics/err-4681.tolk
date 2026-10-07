type BalanceDict = dict
type AssetDict = dict

fun BalanceDict.doSome(self) {}

fun m(d: AssetDict) {
    d.doSome()
}

/**
@compilation_should_fail
@stderr method `doSome` not found for type `AssetDict`
@stderr but it exists for type `BalanceDict`
 */
