fun int8.doSome2() {}
fun int16.doSome2() {}

fun main() {
    int.doSome2();
}

/**
@compilation_should_fail
@stderr call to method `doSome2` for type `int` is ambiguous
@stderr candidate function: `int8.doSome2`
@stderr candidate function: `int16.doSome2`
 */
