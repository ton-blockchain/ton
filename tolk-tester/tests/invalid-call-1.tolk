fun main() {
    return true();
}

/**
@compilation_should_fail
The message is weird now, but later I'll rework error messages anyway.
@stderr cannot apply expression of type int to an expression of type (): cannot unify type () -> ??3 with int
 */
