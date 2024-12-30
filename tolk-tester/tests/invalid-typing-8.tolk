fun failExplicitCastIncompatible(c: cell) {
    return c as slice;
}

/**
@compilation_should_fail
@stderr type `cell` can not be cast to `slice`
 */
