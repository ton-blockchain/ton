type MInt = int;
type MInt2 = MInt;

fun int.check(self) {}
fun MInt.check(self) {}
fun MInt2.check(self) {}
fun `MInt`.`check`(self) {}

/**
@compilation_should_fail
@stderr redefinition of symbol
@stderr err-1226.tolk:7
@stderr err-1226.tolk:5
 */
