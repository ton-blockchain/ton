fun main(arg: Box): int {
    return 0;
}

struct Box<T = Box> {}

/**
@compilation_should_fail
@stderr type `Box` circularly references itself
 */
