struct Point { x: int, y: int }

fun Point<T>.create() {}

/**
@compilation_should_fail
@stderr type `Point` is not generic
@stderr fun Point<T>
 */
