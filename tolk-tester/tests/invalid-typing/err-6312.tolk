enum Color { Red, Green, Blue }
enum Shape { Circle, Square }

fun main(c: Color) {
    c as Shape;     // ok
    match (c) {
        Shape.Circle => {}
    }
}

/**
@compilation_should_fail
@stderr wrong pattern matching: can not compare type `Shape` with match subject of type `Color`
 */
