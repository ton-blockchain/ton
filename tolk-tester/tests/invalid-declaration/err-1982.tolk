contract C {
    storage: Box
}

struct Box<T = Box> {}

fun main(): int {
    return 0;
}

/**
@compilation_should_fail
@stderr type `Box` circularly references itself
 */
