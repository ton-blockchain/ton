fun main() {
    var t = [[[], [], []], [], [[]]];
}

/**
@compilation_should_fail
@stderr type of `[...]` is `array<array<array<unknown>> | array<unknown>>`; probably, it's not what you expected
 */
