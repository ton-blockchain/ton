struct A { x: int }
struct B { y: int }
type U = A | B;

fun U.id(self): self { return self; }
fun U.toA(mutate self) { self = A { x: 1 }; }

fun wrapped_fail() {
    var sh: [U] = [B { y: 7 }];
    if (sh.0 is B) {
        sh.0.id().toA(); // tmp expression
    }
}

/**
@compilation_should_fail
@stderr can not mutate a temporary expression
 */
