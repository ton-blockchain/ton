fun grow<T>(x: T) { return grow([x]); }
fun main() { return grow(1); }

/**
@compilation_should_fail
@stderr instantiation of `grow<T>` exceeded depth limit; it's most likely caused by an infinitely growing type
 */
