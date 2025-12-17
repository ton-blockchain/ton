struct A { a1: int7; a2: int8; a3: int8; }
struct B { b1: int7; b2: int8; b3: int8; }
type AOrB = A | B;

fun main() {
    var m = lazy AOrB.fromSlice("");
    match (m) {
        A => {
            __expect_lazy("[m] skip (int7) load a2");
            match (m) {
                A => {
                    m.a2 + 100;
                }
            }
        }
        B => {
            __expect_lazy("[m] load b1 b2 b3");
            m = m as AOrB;
            match (m) {
                A => {
                    m.a1 + 100;
                }
                B => {
                    m.b2 - 100;
                }
            }
        }
    }
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `m` changes its type inside `match`
 */
