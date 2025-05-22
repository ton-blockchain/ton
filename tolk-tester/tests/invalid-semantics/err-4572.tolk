fun checkCantMutateFieldOfImmutableTuple() {
    val ks = null as [int, [int, [int]]]?;
    ks!.1.1.0 = 10;
}

/**
@compilation_should_fail
@stderr modifying immutable variable `ks`
 */
