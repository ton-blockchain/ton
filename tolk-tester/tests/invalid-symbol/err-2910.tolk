enum Color { Red, Green, Blue }

fun main() {
    Color.Yellow
}

/**
@compilation_should_fail
@stderr member `Yellow` does not exist in enum `Color`
 */
