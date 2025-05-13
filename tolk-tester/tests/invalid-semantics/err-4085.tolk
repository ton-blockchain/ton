struct A {
    value: int;
}

struct B {
    data: slice;
}

fun main() {
    var r1: A | B = A { value: 10 };
    var r2: A | null = { value: 10 };
    var r3: A | int | builder = { value: 10 };

    var r4: A | B = { value: 20 };
}

/**
@compilation_should_fail
@stderr can not detect struct name
@stderr use either `var v: StructName = { ... }` or `var v = StructName { ... }`
@stderr { value: 20 }
 */
