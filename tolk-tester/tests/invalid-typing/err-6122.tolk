enum Color { Red, Green, Blue }
type ColorAlias = Color

fun main(c: Color) {
    match (c) {
        Color.Red => {}
        Color.Green => {}
        ColorAlias.Blue => {}
        else => {}
    }
}

/**
@compilation_should_fail
@stderr `match` already covers all possible enum members, `else` is invalid
@stderr else => {}
 */
