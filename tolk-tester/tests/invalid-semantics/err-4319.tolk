type MInt = int;

fun (int | slice).method(self) {}
fun (MInt|null).method(self) {}
fun int8?.method(self) {}

fun main(p: int8) {
    p.method();
}

/**
@compilation_should_fail
@stderr call to method `method` for type `int8` is ambiguous
@stderr candidate function: `(int|slice).method`
@stderr candidate function: `MInt?.method`
@stderr candidate function: `int8?.method`
*/
