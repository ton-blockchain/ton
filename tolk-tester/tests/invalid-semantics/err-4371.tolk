fun eq<T>(x: T) { return x }

fun int.incrementBy(mutate self, dx: int) { self += dx }

fun test(xy: (int, int)) {
    // ok
    xy.0 = (xy.0 += 10);
    xy.0 += (xy.1 = xy.0);
    xy.0.incrementBy(([xy.1] = [xy.0]).0);
    xy.0.incrementBy(([xy.1] = [xy.0]).0);
    xy.0.incrementBy(([eq([xy.0].0 = xy.0 + 10)].0));

    // error
    var eqVoid = eq<void>;
    eqVoid(xy.0.incrementBy(([xy.1, (xy.0.incrementBy(1), xy.0).1] = [0, 0]).0));
}

/**
@compilation_should_fail
@stderr can not borrow `xy.0` for mutation once again, it is already being mutated by `int.incrementBy`
 */
