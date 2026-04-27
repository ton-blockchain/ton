// @compilation_should_fail

type PairShape = [int, int]

// @stderr can not convert type `PairShape` to return type `array<int>`
fun makePair(): array<int> {
    return PairShape [1, 2]
}

enum Color { Red, Green, Blue }

// @stderr can not apply operator `+` to `int` and `Color`
fun sumEnum() {
    return 1 + Color.Blue;
}
