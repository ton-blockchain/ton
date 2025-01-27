fun failAssignNullToTensor() {
    var ab = (1, 2);
    ab = null;
    return ab;
}

/**
@compilation_should_fail
@stderr can not assign `null` to variable of type `(int, int)`
 */
