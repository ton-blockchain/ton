struct A { x: int }
struct B { x: int, y: int }

fun poison(mutate b: B) {}
fun useTwo(mutate p: int, mutate q: int) {}

fun main() {
    var a = A { x: 0 };
    poison(mutate a);
    // after this, `a` becomes `B` (it's a collected error of mutate)
    // but field `.y` is resolved, and later fired in a borrow checker
    useTwo(mutate a.y, mutate a.y);
}

/**
@compilation_should_fail
@stderr can not pass `A` to `B`
@stderr can not pass `A` to mutate `B`, because mutation is not type compatible
@stderr can not borrow `a.1` for mutation once again, it is already being mutated by `useTwo`
 */
