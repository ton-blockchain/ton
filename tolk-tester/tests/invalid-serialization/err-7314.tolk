struct A { a1: int8; }
struct B { b1: int8; }
type AOrB = A | B;

fun main() {
    var o = lazy AOrB.fromSlice(createEmptySlice());
    val cb = fun() {
        match (o) {
            A => { }
            B => { }
        }
    }
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `o` is used in a non-lazy manner
 */
