struct Point {
    x: int;
    y: int;
}

struct Coord2 {
    p1: Point?;
    p2: Point;
}

fun increment(mutate x: int) {
    x += 1;
}

fun checkCantMutateFieldOfImmutableTuple(p1: Point?, p2: Point) {
    val c2: Coord2 = { p1, p2 };
    (c2!.p1!).x.increment();
}

/**
@compilation_should_fail
@modifying immutable variable `c2`
 */
