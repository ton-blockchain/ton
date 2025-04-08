type MInt32 = int32;

fun testAssignBetweenDifferentIntN(op: int32, qid: uint64) {
    op = qid as MInt32;    // ok
    op = qid;
}

/**
@compilation_should_fail
@stderr can not assign `uint64` to variable of type `int32`
@stderr op = qid;
 */
