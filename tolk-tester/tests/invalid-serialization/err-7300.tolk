struct A { a1: int7; a2: int8; a3: int8; }
struct B { b1: int7; b2: int8; b3: int8; }
type AOrB = A | B;

fun f() {
    val o = lazy AOrB.fromSlice("");
    __expect_lazy("");
    return (o is A) ? o.a1 : o.b1;
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `o` is used in a non-lazy manner
@stderr hint: lazy union may be used only in `match` statement
 */
