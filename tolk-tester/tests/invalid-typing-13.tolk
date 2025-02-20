fun failAssignToInvalidTupleIndex() {
    var ab = [1, 2];
    ab.100500 = 5;
}

/**
@compilation_should_fail
@stderr invalid tuple index, expected 0..1
 */
