fun (T|int).equals<U>(self, v: U) { return self == v; }
fun T?.equals<U>(self, v: U) { return self == v; }

fun main() {
    (10 as int?).equals(20);
}

/**
@compilation_should_fail
@stderr error: call to method `equals` for type `int?` is ambiguous
@stderr candidate function: `(T|int).equals<U>` with T=`null`
@stderr candidate function: `T?.equals<U>` with T=`int`
@stderr (10 as int?).equals(20);
 */
