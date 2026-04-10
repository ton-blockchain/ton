struct S { v: () -> int; }

fun cantCreateIncorrectTypeToField() {
    var s: S = { v: beginCell };
}

/**
@compilation_should_fail
@stderr can not assign `() -> builder` to field of type `() -> int`
@stderr v: beginCell
 */
