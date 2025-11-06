enum Color { Red, Green, Blue }

fun main() {
    return Color.Red<int>
}

/**
@compilation_should_fail
@stderr type arguments not expected here
 */
