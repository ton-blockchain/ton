struct A { }
struct B { }
type AOrB = A | B;

@inline
fun AOrB.getSecondField(self): int8 {
    return match (self) {
        A => {}
        B => {}
    };
}

fun main(a: A, b: B) {
    a.getSecondField();
}

/**
@compilation_should_fail
@stderr method `getSecondField` not found for type `A`
@stderr (but it exists for type `AOrB`)
 */