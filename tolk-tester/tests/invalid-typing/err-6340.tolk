fun main() {
    var a = [1, []];
}

/**
@compilation_should_fail
@stderr type of `[...]` is `array<int | array<unknown>>`; probably, it's not what you expected
 */
