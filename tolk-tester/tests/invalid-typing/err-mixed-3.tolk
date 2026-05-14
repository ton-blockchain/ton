// @compilation_should_fail

type PairShape = [int, int]
type One = [int]

// @stderr can not convert type `PairShape` to return type `array<int>`
// @stderr can not convert `string` to `int`
fun makePair(): array<int> {
    var value = PairShape [1, "not an int"];
    return PairShape [1, 2]
}

// @stderr invalid `[...]` constructor for `[int]`: expected 1 items
fun shapeLenMismatch() {
    One [1, 2]
}

enum Color { Red, Green, Blue }

// @stderr can not apply operator `+` to `int` and `Color`
fun sumEnum() {
    return 1 + Color.Blue;
}
