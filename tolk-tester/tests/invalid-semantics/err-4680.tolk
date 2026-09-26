type BalanceDict = dict

fun BalanceDict.doSome(self) {}

fun m(d: dict) {
    d.doSome()
}

/**
@compilation_should_fail
@stderr method `doSome` not found for type `dict`
@stderr but it exists for type `BalanceDict`
 */
