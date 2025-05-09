fun main() {
    var f = stringHexToSlice;
    f("asdf");
}

/**
@compilation_should_fail
@stderr can not get reference to this function, it's compile-time only
 */
