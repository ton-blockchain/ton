struct Pair<A,B> {
    first: A;
    second: B;
}

type PairAlias<A,B> = Pair<A,B>;

fun PairAlias<T>.create() {}

/**
@compilation_should_fail
@stderr type `PairAlias<A, B>` expects 2 type arguments, but 1 provided
 */
