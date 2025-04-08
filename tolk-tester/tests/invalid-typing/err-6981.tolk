fun getTwo<X>(): X { return 2; }

fun cantDeduceNonArgumentGeneric() {
    var t1: [int] = [0];
    t1.0 = getTwo();   // ok
    var t2 = createEmptyTuple();
    t2.tuplePush(0);
    t2.0 = getTwo();   // error, can't decude X
}

/**
@compilation_should_fail
@stderr can not deduce X for generic function `getTwo<X>`
@stderr t2.0 = getTwo();
 */
