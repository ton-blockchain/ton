struct Pair<A,B> {
    first: A;
    second: B;
}

fun Pair<A,B>.compareWith<U,V>(self, f: U, s: V) {
    return self.first == f && self.second == s;
}

fun main(p: Pair<address,slice>) {
    p.compareWith<address?,slice?>(null, null);
}

/**
@compilation_should_fail
@stderr can not apply operator `==` to `address` and `address?`
@stderr in function `Pair<address,slice>.compareWith<address?,slice?>`
@stderr check on `null` first, or use unsafe operator `!`
 */
