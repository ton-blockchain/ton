fun eq<T>(x: T) { return x }

fun int.incrementBy(mutate self, dx: int) { self += dx }

fun test(x: int, y: int) {
    // ok
    x = (x += 10);
    x += (y = x);
    x.incrementBy(([y] = [x]).0);
    x.incrementBy(([y] = [x]).0);
    x.incrementBy(([eq([x].0 = x + 10)].0));

    // error
    x.incrementBy(([y, x] = [0, 0]).0);
}

/**
@compilation_should_fail
@stderr can not borrow `x` for mutation once again, it is already being mutated by `int.incrementBy`
@stderr ([y, x] = [0, 0]).0 
 */
