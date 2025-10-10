enum Color { Red, Green, Blue }
type ColorAlias = Color

fun main(c: Color) {
    match (c) {
        Color.Red => {}
        Color.Green => {}
        ColorAlias.Red => {}
        else => {}
    }
}

/**
@compilation_should_fail
@stderr wrong pattern matching: duplicated enum member in `match`
@stderr ColorAlias.Red
 */
