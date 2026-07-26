fun main(x: (int, int) | (slice, int)) {
    if (x is (int, int)) {
    }
    return x.0;
}

/**
@compilation_should_fail
@stderr field `0` doesn't exist in type `(int, int) | (slice, int)`
@stderr return x.0
 */
