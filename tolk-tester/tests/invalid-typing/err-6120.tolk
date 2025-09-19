enum Color { Red, Green, Blue }

fun main(c: Color) {
    match (c) {
        Color.Red => {}
        else => {}
    }
    match (c) {
        Color.Green => {}
    }
}

/**
@compilation_should_fail
@stderr `match` does not cover all possible enum members; missing members are: Red, Blue
 */
