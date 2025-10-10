enum Color { Red, Green, Blue, }

fun main() {
    Color.Red = 123 as Color;
}

/**
@compilation_should_fail
@stderr modifying immutable constant
 */
