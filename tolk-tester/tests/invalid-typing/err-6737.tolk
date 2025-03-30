fun testAssignBetweenDifferentIntN(op: int32, qid: uint64) {
    op = qid as int32;    // ok
    op = qid;
}

/**
@compilation_should_fail
@stderr can not assign `uint64` to variable of type `int32`
@stderr op = qid;
 */
