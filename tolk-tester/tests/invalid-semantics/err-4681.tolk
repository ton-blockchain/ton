type BalanceDict = dict

fun dict.doSome(self) {}
fun BalanceDict.doSome(self) {}

fun m(d: BalanceDict) {
    d.doSome()
}

/**
@compilation_should_fail
@stderr call to method `doSome` for type `BalanceDict` is ambiguous
@stderr candidate function: `dict.doSome`
@stderr candidate function: `BalanceDict.doSome`
 */
