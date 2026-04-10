fun int.realValue(self): int8 { return 1 }
fun int?.realValue(self): uint256 { return 1 }

fun main() {
    (5 as int8).realValue();
}

/**
@compilation_should_fail
@stderr call to method `realValue` for type `int8` is ambiguous
@stderr candidate function: `int.realValue`
@stderr candidate function: `int?.realValue`
 */
