struct All<A> {
    f1: A;
    f2: A;
    f3: A;
}

fun testInferGenericOneByOne(x: int?) {
    // ok
    __expect_type(All {
        f1: x,
        f2: 5,
        f3: null
     }, "All<int?>");
    // err
    All {
        f1: x,
        f2: null,
        f3: beginCell()     // type checker error
    };
}

/**
@compilation_should_fail
@stderr can not assign `builder` to field of type `int?`
@stderr f3: beginCell()
 */

