enum Color { Red, Green, Blue }

fun main() {
    match (2 as int8) {
        Color.Red => {}
    }
}

/**
@compilation_should_fail
@stderr wrong pattern matching: can not compare type `Color` with match subject of type `int8`
 */
