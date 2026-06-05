struct Pair<T1, T2> {
    first: T1
    second: T2
}

fun Pair<T1, T2>.create(first: T1, second: T2): Pair<T1, T2> {
    return { first, second }
}

fun main() {
    Pair.create(1, true);
}

/**
@compilation_should_fail
@stderr can not deduce type arguments for `Pair<T1, T2>`, provide them manually
 */
