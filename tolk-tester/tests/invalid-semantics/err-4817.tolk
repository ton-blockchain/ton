fun mkPair<T, U>(): [T?, U?] {
    throw 777;
}

fun main() {
    var [a: int?, b] = null ?? mkPair();
    return (b is int) ? 1 : 0;
}

/**
@compilation_should_fail
@stderr can not deduce U for generic function `mkPair<T, U>`
 */
