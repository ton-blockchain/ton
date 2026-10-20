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

type V15UserIdInUnion = int
type V15OwnerIdInUnion = int
type V15SomeThing = V15UserIdInUnion | slice

// @stderr wrong pattern matching: use `V15UserIdInUnion` instead of `V15OwnerIdInUnion`
fun aliasV15UnionIsSpelling(v: V15SomeThing) {
    return v is V15OwnerIdInUnion;
}

struct V15MsgA {
    value: int
}

type V15A1 = V15MsgA
type V15A2 = V15A1
type V15A3 = V15A2
type V15SomeMsg = V15A2 | slice

// @stderr wrong pattern matching: use `V15A2` instead of `V15A3`
fun aliasV15UnionMatchChain(v: V15SomeMsg) {
    return match (v) {
        V15A3 => 1,
        slice => 2,
    };
}
