type MTensor = (int, int);

fun failAssignNullToTensor() {
    var ab: MTensor = (1, 2);
    ab = null;
    return ab;
}

/**
@compilation_should_fail
@stderr can not assign `null` to variable of type `MTensor`
 */
