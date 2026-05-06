struct Pair<X, Y> {
    first: X
    second: Y
}

fun Pair<X, Y>.packToBuilder(self, mutate b: builder) {}
fun Pair<X, int8>.packToBuilder(self, mutate b: builder) {}
fun Pair<string, Y>.packToBuilder(self, mutate b: builder) {}

fun main() {
    var p1 = Pair<int32, bool> { first: 123, second: true };
    p1.toCell();
    var p2 = Pair<int32, int8> { first: 123, second: 8 };
    p2.toCell();
    var p3 = Pair<string, int16> { first: "asdf", second: 16 };
    p3.toCell();

    var pE = Pair<string, int8> { first: "asdf", second: 8 };
    pE.toCell();
}

/**
@compilation_should_fail
@stderr ambiguous method, both `Pair<X, int8>.packToBuilder` and `Pair<string, Y>.packToBuilder` are applicable
 */
