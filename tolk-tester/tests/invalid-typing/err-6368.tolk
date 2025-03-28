fun cantAssignSizesMismatch() {
    var [x, y] = [2, 3, 4];
    x + y;
}

/**
@compilation_should_fail
@stderr can not assign `[int, int, int]`, sizes mismatch
 */
