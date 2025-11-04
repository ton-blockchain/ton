enum Color { Red, Green, Blue }

fun main(c: Color) {
    c == 1
}

/**
@compilation_should_fail
@stderr can not apply operator `==` to `Color` and `int`
 */
