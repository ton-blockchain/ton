struct A { a1: int8; }
struct B { b1: int8; }
type AOrB = A | B;

fun mutateUnion(mutate x: AOrB) {
}

fun lazy_case() {
    var o = lazy AOrB.fromSlice(createEmptySlice());
    match (o) {
        A => { return mutateUnion(mutate o); }
        B => { return mutateUnion(mutate o); }
    }
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `o` changes its type inside `match`
 */
