struct Point {
    x: int8;
    y: int8;
}

fun cantAssignToLazyVar() {
    var p = lazy Point.fromSlice(createEmptySlice());
    (p, _) = ({ x: 10, y: 20 }, 100);
}

/**
@compilation_should_fail
@stderr can not assign to a lazy variable `p`
@stderr (p, _) =
 */
