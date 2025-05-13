fun getTwo<X>(): X { return 2 as X; }

fun cantDeduceNonArgumentGeneric() {
    var t1: [int] = [0];
    t1.0 = getTwo();   // ok
    var t2 = createEmptyTuple();
    t2.push(0);
    t2.0 = getTwo();   // deduced X = unknown
}

/**
@compilation_should_fail
@stderr in function `getTwo<unknown>`
@stderr type `int` can not be cast to `unknown`
 */
