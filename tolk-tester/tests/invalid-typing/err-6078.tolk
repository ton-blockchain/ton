struct S { v: int; }

fun cantAssignIncorrectTypeToField() {
    var s: S = { v: 0 };
    s.v = createEmptyTuple();
}

/**
@compilation_should_fail
@stderr can not assign `array<unknown>` to field of type `int`
 */
