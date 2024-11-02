get seqno(self: int) {
    return 0;
}

/**
@compilation_should_fail
@stderr get methods can't have `mutate` and `self` params
 */
