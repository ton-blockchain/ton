
fun main() {
    var a = 0 as int | slice;
    var bb = match (a) {
    };
    __expect_type(a, "int | slice");
    return a;
}

/**
@compilation_should_fail
@stderr empty `match` can't be used as expression
@stderr var bb = match
 */
