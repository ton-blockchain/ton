struct Pair<A,B> {
    first: A;
    second: B;
}

fun Pair<A,B>.compareWith<U,V>(self, f: U, s: V) {
    return self.first == f && self.second == s;
}

fun main(p: Pair<int,slice>) {
    p.compareWith<int?,slice?>(null, null);
}

/**
@compilation_should_fail
@stderr can not apply operator `==` to `int` and `int?`
@stderr in function `Pair<int,slice>.compareWith<int?,slice?>`
 */
