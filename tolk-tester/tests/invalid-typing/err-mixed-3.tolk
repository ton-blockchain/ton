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

fun int.increment(mutate self) {
    self += 1;
}

// @stderr can not mutate a temporary expression
fun cantMutateTensorElem(a: int, b: int) {
    (a, b).0.increment();
}

type MInt = int
type Pair2_v1 = | (int, int)
type Pair2_v2 = (MInt, MInt)

// @stderr can not convert type `Pair2_v2` to ternary result type `Pair2_v1`
const INVALID_TERNARY = 10>3 ? (1, 2) as Pair2_v1 : (1, 2) as Pair2_v2
