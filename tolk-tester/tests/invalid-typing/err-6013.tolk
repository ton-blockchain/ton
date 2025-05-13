struct Pair<A,B> {
    first: A;
    second: B;
}

fun Pair<A,B>.demoInvalid(self) {
    return self.first == (null as int?);
}

fun main(p: Pair<int,slice>) {
    p.demoInvalid();
}

/**
@compilation_should_fail
@stderr can not apply operator `==` to `int` and `int?`
@stderr in function `Pair<int,slice>.demoInvalid`
 */
