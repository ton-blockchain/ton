fun f(a: int = 0, b: int) {

}

fun main() {
    f(1, 2);        // ok
    return f();
}

/**
@compilation_should_fail
@stderr too few arguments in call to `f`, expected 2, have 0
@stderr f();
 */
