fun cantAssignIntToTensor() {
    var (x, y) = 2;
    x + y;
}

/**
@compilation_should_fail
@stderr can not assign `int` to a tensor
 */
