struct A { }
struct B { }
type C = A | B;

@noinline
fun C.onInternalMessage(self) {
    return match (self) {
        A{ => 1,
            B => 2,
        };
    }

    fun main() {}

/**
@compilation_should_fail
@stderr  expected <type> or <expression> in `match` before `=>`
 */
