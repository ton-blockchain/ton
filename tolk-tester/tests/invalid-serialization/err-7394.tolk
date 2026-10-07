struct G<T> {
    x: int8
}

fun G<T>.packToBuilder<U>(self, mutate b: builder) {
    b.storeInt(self.x, 8);
}

fun main() {
    return G<int> { x: 1 }.toCell();
}

/**
@compilation_should_fail
@stderr can not deduce U for generic function `G<T>.packToBuilder<U>`
@stderr .toCell()
*/
