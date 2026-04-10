struct Wrapper<T> {
    value: T;
}

type MyInt = int;

fun cantAssignFieldsOfDifferentTypes(
    w1: Wrapper<int>, w2: Wrapper<int8>, w3: Wrapper<MyInt>, w4: Wrapper<()>
) {
    w1.value = w2.value;        // ok
    w1.value = w3.value;        // ok
    w3.value = w2.value;        // ok

    w1.value = w4.value;
}

/**
@compilation_should_fail
@stderr can not assign `()` to field of type `int`
@stderr w1.value = w4.value
 */
