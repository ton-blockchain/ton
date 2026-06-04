struct Point {
    x: int8;
    y: int8;
}

fun cantAssignToLazyVar() {
    var p = lazy Point.fromSlice(createEmptySlice());
    var cell = (p = p).toCell();
}

/**
@compilation_should_fail
@stderr can not assign to a lazy variable `p`
@stderr p = p
 */
