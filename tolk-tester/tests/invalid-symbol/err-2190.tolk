enum Color {
    Red,
    Green,
    Blue,
}

fun main(c: Color) {
    c.Red
}

/**
@compilation_should_fail
@stderr field `Red` doesn't exist in type `Color` 
 */
