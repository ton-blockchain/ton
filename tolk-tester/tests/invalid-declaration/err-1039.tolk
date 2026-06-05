
enum Color { Red = Color.Red }

/**
@compilation_should_fail
@stderr enum `Color` initializers circularly references itself
 */
