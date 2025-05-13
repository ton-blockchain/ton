fun takeNullableTensor(mutate ij: (int, int)?) { }

fun testSmartCastsDropAfterMutate() {
    var x: (int, int)? = (1, 2);
    return x.0;     // ok
    takeNullableTensor(mutate x);
    return x.1;     // error
}

/**
@compilation_should_fail
@stderr field `1` doesn't exist in type `(int, int)?`
@stderr return x.1
 */
