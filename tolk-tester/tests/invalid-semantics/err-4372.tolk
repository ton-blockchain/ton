struct Point {
    x: int
    y: int
}

fun Point.copy(self) { return self }
fun Point.mut(mutate self) { return self }

fun test(p: Point) {
    // ok
    p.x += (p.copy().x = 5);
    p.x += (p.copy().x += 5);

    // err
    p.x += (p.mut().x = 5);
}

/**
@compilation_should_fail
@stderr can not borrow `p` for mutation, because `p.x` is already being mutated by `assignment operator` 
 */
