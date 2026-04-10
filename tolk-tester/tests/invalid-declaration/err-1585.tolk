struct B { a: A }
struct A { b: BAlias }

type BAlias = B;

fun test(a: A) {
    a.b;
    a.b.a;
}

/**
@compilation_should_fail
(error message not stable, sometimes about A, sometimes B, it's okay, they are in a hashmap)
@stderr size is infinity due to recursive fields
 */
