type MInt = int;
type IntOrSlice = MInt|slice;

fun (MInt | slice).method(self) {}
fun (slice | MInt).method(self) {}
fun IntOrSlice.method(self) {}

fun main(p: MInt|slice) {
    p.method();
}

/**
@compilation_should_fail
@stderr call to method `method` for type `MInt | slice` is ambiguous
@stderr candidate function: `(MInt | slice).method`
@stderr candidate function: `(slice | MInt).method`
@stderr_avoid candidate function: `IntOrSlice.method`
*/
