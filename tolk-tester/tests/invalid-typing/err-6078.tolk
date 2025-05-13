struct S { v: int; }

fun cantAssignIncorrectTypeToField() {
    var s: S = { v: 0 };
    s.v = createEmptyTuple();
}

/**
@compilation_should_fail
@stderr can not assign `tuple` to field of type `int`
 */
