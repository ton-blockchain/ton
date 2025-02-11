fun eq<X>(v: X) {}

fun cantDeduceWhenNotInferred() {
    // at type inferring (before type checking) they are unknown
    var (x, y) = 2;

    eq(x as int);   // ok (since execution doesn't reach type checking)
    eq<int>(x);     // ok (since execution doesn't reach type checking)
    eq(x);
}

/**
@compilation_should_fail
@stderr in function `cantDeduceWhenNotInferred`
@stderr can not deduce X for generic function `eq<X>`
@stderr eq(x);
 */
