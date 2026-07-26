struct Left {}
struct Right {}

type A<T> = Left | Right
type B<T> = A<T>

fun A<T>.packToBuilder(self, mutate b: builder) {
}

fun A<T>.unpackFromSlice(mutate s: slice): A<T> {
    return Left {}
}

fun main(s: slice) {
    val u = lazy B<int>.fromSlice(s);
    match (u) {
        Left => {}
        Right => {}
    }
}

/**
@compilation_should_fail
@stderr `lazy` is not applicable to `B<int>`, because it overrides packToBuilder/unpackFromSlice
 */
