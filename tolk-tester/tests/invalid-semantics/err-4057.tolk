fun int16.plus1(self) { return self + 100; }
fun int8.plus1(self) { return self + 1; }

fun main() {
    (10 as int16).plus1();  // ok
    (10 as int8).plus1();   // ok

    10.plus1();
}

/**
@compilation_should_fail
@stderr call to method `plus1` for type `int` is ambiguous
@stderr candidate function: `int16.plus1`
@stderr candidate function: `int8.plus1`
@stderr 10.plus1
 */
