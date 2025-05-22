type MInt = int;
type IntOrSlice = MInt|slice;

fun (int | slice).method(self) {}
fun (slice | MInt).method(self) {}
fun IntOrSlice.method(self) {}

fun main(p: int|slice) {
    p.method();
}

/**
@compilation_should_fail
@stderr call to method `method` for type `int | slice` is ambiguous
@stderr candidate function: `(int|slice).method`
@stderr candidate function: `(slice|MInt).method`
@stderr candidate function: `IntOrSlice.method`
*/
