fun main() {
    var test = builder.storeSlice(1, 5);
}

/**
@compilation_should_fail
@stderr can not pass `int` to `builder`
 */
