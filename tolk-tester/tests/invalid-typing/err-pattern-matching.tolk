// @compilation_should_fail
// various tests when `match` is used incorrectly (not all! there are lots of independent err-xxxx)

enum Color { Red, Green, Blue }
enum Shape { Circle, Square }
type ColorAlias = Color

// @stderr `match` does not cover all possible enum members; missing members are: Red, Blue
fun errMatchNotCoveringAllEnum(c: Color) {
    match (c) {
        Color.Red => {}
        else => {}
    }
    match (c) {
        Color.Green => {}
    }
}

// @stderr wrong pattern matching: duplicated enum member in `match`
fun errMatchDuplicatedEnumMember(c: Color) {
    match (c) {
        Color.Red => {}
        Color.Green => {}
        ColorAlias.Red => {}
        else => {}
    }
}

// @stderr `match` already covers all possible enum members, `else` is invalid
fun errMatchElseWhenAllCovered(c: Color) {
    match (c) {
        Color.Red => {}
        Color.Green => {}
        ColorAlias.Blue => {}
        else => {}
    }
}

// @stderr wrong pattern matching: can not compare type `int` with match subject of type `Color`
fun errMatchIntPatternForEnum(c: Color) {
    match (c) {
        2 => {}
    }
}

// @stderr wrong pattern matching: can not compare type `Color` with match subject of type `int8`
fun errMatchEnumPatternForInt() {
    match (2 as int8) {
        Color.Red => {}
    }
}

// @stderr wrong pattern matching: can not compare type `Shape` with match subject of type `Color`
fun errMatchWrongEnumType(c: Color) {
    match (c) {
        Shape.Circle => {}
    }
}


struct Wrapper<T> {
    field: T;
}

type MyInt = int;
type WrapperAlias<TTT> = Wrapper<TTT>;

// @stderr wrong pattern matching: duplicated `WrapperAlias<int>`
fun duplicateInStrangeUnion(w: int | WrapperAlias<int> | WrapperAlias<int8>) {
    match (w) {
        MyInt => {}
        Wrapper<MyInt> => {}
        WrapperAlias<int> => {}
    }
}

struct Inc5 { v: int32 }

type Msg5 = Inc5

// @stderr `match` contains only `else`, but no variants
fun onlyElseBranch(s: slice) {
    val msg = lazy Msg5.fromSlice(s);
    match (msg) {
        else => throw 0xFFFF
    }
}

type IntOrSlice = int | slice;

// @stderr wrong pattern matching: union types are not allowed, use concrete types in `match`
// @stderr IntOrSlice => 1
fun unionTypesNotAllowedInMatch(a: int | slice) {
    match (a) {
        IntOrSlice => 1,
    };
}
