enum Color { Red, Green, Blue }

fun main(c: Color) {
    match (c) {
        2 => {}
    }
}

/**
@compilation_should_fail
@stderr wrong pattern matching: can not compare type `int` with match subject of type `Color`
 */
