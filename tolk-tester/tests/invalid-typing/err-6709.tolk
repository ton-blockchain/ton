struct A { id: int; }
struct B { id: int; }
type AAlias = A;

fun cantAssignDifferentStructures(a: A) {
    var a2: AAlias = a;     // ok
    var b: B = a;
}

/**
@compilation_should_fail
@stderr can not assign `A` to variable of type `B`
 */
