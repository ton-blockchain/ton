fun takeNullableTensor(mutate ij: (int, int)?) { }

fun testSmartCastsDropAfterMutate() {
    var x: (int, int)? = (1, 2);
    return x.0;     // ok
    takeNullableTensor(mutate x);
    return x.1;     // error
}

/**
@compilation_should_fail
@stderr type `(int, int)?` is not indexable
@stderr return x.1
 */
