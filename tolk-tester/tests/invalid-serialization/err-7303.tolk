type PairInt8 = (int8, int8);

fun f() {
    var i = lazy PairInt8.fromSlice(stringHexToSlice("0102"));
    return (i.0, i.1);
}

/**
@compilation_should_fail
@stderr `lazy` is applicable to structs, not to `PairInt8`
 */
