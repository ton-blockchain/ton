struct S {
    a: uint8;
    `(tail)`: uint8;
    b: uint8;
}

fun main() {
    var s = lazy S.fromSlice(stringHexToSlice("010203"));
    return s.toCell().beginParse().remainingBitsCount();
}

/**
@compilation_should_fail
@stderr this is a reserved field name, it conflicts with `lazy` internals
@stderr `(tail)`
 */
