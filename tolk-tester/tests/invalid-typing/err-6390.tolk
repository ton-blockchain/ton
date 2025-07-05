fun main(k: (int, slice)) {
    var (a, b, c: int?) = k;
}

/**
@compilation_should_fail
@stderr can not assign `(int, slice)`, sizes mismatch
 */
