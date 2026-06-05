struct MaybeJust<T> {
    value: T
}

struct MaybeNothing

type Maybe<T> = MaybeJust<T> | MaybeNothing

fun Maybe<T>.none(): Maybe<T> {
    return MaybeNothing{}
}

fun main() {
    var j: MaybeJust<int> = Maybe.none();
}

/**
@compilation_should_fail
@stderr can not deduce type arguments for `Maybe<T>`, provide them manually
 */
