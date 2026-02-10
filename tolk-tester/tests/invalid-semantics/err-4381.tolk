fun failAssignToInvalidTupleIndex() {
    var ab = [1, 2] as [int, int];
    ab.100500 = 5;
}

/**
@compilation_should_fail
@stderr invalid array index, expected 0..1
 */
