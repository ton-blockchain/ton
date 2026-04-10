fun with2Params(x: int, y: int) {

}

fun main() {
    return with2Params(1);
}

/**
@compilation_should_fail
@stderr too few arguments in call to `with2Params`, expected 2, have 1
 */
