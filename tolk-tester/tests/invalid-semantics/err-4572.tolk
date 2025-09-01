fun eq<T>(v: T) { return v }

fun checkCantMutateFieldOfImmutableTuple() {
    val ks = null as [int, [int, [int]]]?;
    eq(ks!.1.1 = [10]).0 = 10;
}

/**
@compilation_should_fail
@stderr modifying immutable variable `ks`
 */
