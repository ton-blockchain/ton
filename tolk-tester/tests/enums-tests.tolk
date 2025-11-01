const SIX = 6

enum EEmpty {}

enum Color {
    Red,
    Green,
    Blue
}

enum InvertedColor {
    Red = Color.Blue,
    Green = Color.Green,
    Blue = Color.Red,
    OneTon = ton("1"),
}

type ColorAlias = Color

struct WithColor {
    c1: Color
    c2: Color = Color.Green
}

enum EPartialAssigned {
    M100 = -100,
    M99,
    P1 = (1 + 0) << 0;
    P2 = ((+1 + 1) << 1) / 2;
    P3
    P4
    P5 = 0b101,
    P6 = SIX;
    Max = 0x7FFFFFFF,
}

enum Err {
    TooLowId = 0x100,
    TooHighId,
    DisabledId,
}

struct Box<T> { 
    value: T 
}

fun checkIsBlue(c: Color) {
    return c == Color.Blue
}

fun Color.isBlue(self) {
    return self == Color.Blue
}

fun Color.createBlue() {
    return Color.Blue
}

const C_BLUE = Color.Blue
const C_TEN = 5 * (Color.Green as int) * 2

// these methods do not bother Color.Red and other
fun T.Red() { return 123 }
fun T.Max() { return -C_TEN }
fun int.Max(self) { return self }
fun Color.Max(): EPartialAssigned { return ((EPartialAssigned.Max as int) + 1) as EPartialAssigned }

@noinline
fun checkIsColor<T>(obj: Color | T) { return obj is Color }

@inline_ref
fun Color.mapToNumber(self) {
    // match over enum is exhaustive, it's like "if Red {} else if Green {} else if Blue {} else throw"
    return match (self) {
        Color.Red => 100,
        Color.Green => 200,
        Color.Blue => 300,
        // there is a hidden "else" in case stack is malformed
    }
}


@method_id(101)
fun test1() {
    var c = Color.Red;
    
    return (
        checkIsBlue(Color.Blue), checkIsBlue(c), checkIsBlue(Color.createBlue()),
        Color.Blue.isBlue(), Color.Red.isBlue(), Color.createBlue()!!.isBlue(), C_BLUE.isBlue(),
        Color.createBlue()
    );
}

@method_id(102)
fun test2(c: Color) {
    return (c is Color, c !is Color, c is EPartialAssigned, c is EEmpty, c is int, c is int8);
}

@method_id(103)
fun test3() {
    assert (EPartialAssigned.P4 as int == 4) throw 123;
    assert (EPartialAssigned.M99 as int <= 0) throw 123;
    assert (EPartialAssigned.P3 == (3 as EPartialAssigned)) throw 123;
    assert (EPartialAssigned.P3 != (4 as EPartialAssigned)) throw 123;
    return (
        EPartialAssigned.M100,
        EPartialAssigned.M99,
        EPartialAssigned.P1,
        EPartialAssigned.P2,
        EPartialAssigned.P3,
        EPartialAssigned.P4,
        EPartialAssigned.P5,
        EPartialAssigned.P6,
        EPartialAssigned.Max,
    )
}

@method_id(104)
fun test4(chooseGreen: bool) {
    var c: ColorAlias = chooseGreen ? Color.Green : ColorAlias.Blue;
    return (c is Color, (c!) is ColorAlias, ColorAlias.Blue, ColorAlias.createBlue().isBlue(), c);
} 

@method_id(105)
fun test5() {
    return (int.Red(), int8.Max(), 456.Max(), EPartialAssigned.Max, Color.Max());
}

@method_id(106)
fun test6(c: Color) {
    val p = c as EPartialAssigned;
    val e = c as EEmpty;
    match (e) { EEmpty => {} }
    return (p, e, c as int, c as int?, 100 as Color, -100 as EEmpty);
}

@method_id(107)
fun test7() {
    return (
        WithColor{c1: Color.Red},
        WithColor{c1: -100 as Color, c2: Color.createBlue()}
    )
}
    
@method_id(108)
fun test8() {
    var (c0: Color | int, c1: Color | int) = (Color.Red, 123);
    var (u1, u2) = (Color.Red as Color | int, 0 as Color | int);
    var (b1, b2, b3) = (Color.Red as Color | EPartialAssigned | builder, EPartialAssigned.Max as ColorAlias | builder | EPartialAssigned, beginCell() as builder | Color | EPartialAssigned);
    return (
        checkIsColor(u1), checkIsColor(u2), 
        checkIsColor(b1), checkIsColor(b2), checkIsColor(b3),
        b2 is EPartialAssigned, 
        u1,
        match (u1) { int => -100, Color => -200 }
    )
}

@method_id(109)
fun test9() {
    var invalidEx = 0;
    try { invalidEx = (99 as Color).mapToNumber() }
    catch (ex) { invalidEx = ex }
    
    return (
        Color.Red.mapToNumber(), Color.Green.mapToNumber(), Color.Blue.mapToNumber(),
        invalidEx
    );
}

@method_id(110)
fun test10(c: Color) {
    var t = createEmptyTuple();
    match (c) {
        Color.Red => { t.push(1) }
        else => { t.push(-100) }
    }
    match (c) {
        Color.Green => { t.push(2) }
        Color.Blue => { t.push(3) }
        Color.Red => { t.push(1) }
    }
    match (c is Color) {
        true => { t.push(-100) }
    }
    match (c) {
        Color => { t.push(88) }
    }
    match (c) {
        Color.Blue => { t.push(3) }
        Color.Green => { t.push(2) }
        else => { t.push(-100) }
    }
    match (c) {
        Color.Red => { t.push(1) }
        Color.Blue => { t.push(3) }
        Color.Green => { t.push(2) }
    }
    return t;
}

@method_id(111)
fun test11(b1Value: Color) {
    var b1: Box<Color> = { value: b1Value };
    var b2 = Box<Color> { value: Color.Green };
    var b3 = Box { value: Color.Blue };
    return (b1.value == b2.value, b1.value != b2.value, b1.value == b3.value, b1.value != b3.value)
}

@method_id(112)
fun test12(c1: Color, c2: Color) {
    var t = createEmptyTuple();

    match (c1) {
        Color.Red =>   { t.push(10 + c2.mapToNumber()) }
        Color.Green => { t.push(20 + c2.mapToNumber()) }
        Color.Blue =>  { t.push(30 + c2.mapToNumber()) }
    }

    match (c2) {
        Color.Red => {
            match (c1) {
                Color.Red   => t.push(100),
                Color.Blue  => t.push(300),
                Color.Green => t.push(200),
            }
        }
        Color.Blue => match (c1) {
                Color.Green => t.push(210),
                Color.Red   => t.push(110),
                else        => t.push(310),
            },
        Color.Green => match (c1) {
                Color.Green => t.push(220),
                Color.Red   => t.push(120),
                Color.Blue  => t.push(320),
            }
    }

    try { t.push(123); t.push((999 as Color).mapToNumber()); }
    catch {}
    return t;
}

@method_id(113)
fun test13(x: int) {
    try {
        if (x < 0) { throw Err.TooLowId }
        assert (x < 100) throw Err.TooHighId;
        assert (x != 10, Err.DisabledId);
        return (0, false);
    } catch (excno) {
        return (excno, excno as Err == Err.TooLowId);
    }
}

@method_id(114)
fun test14(x: InvertedColor) {
    __expect_type(InvertedColor.Red, "InvertedColor");
    return (InvertedColor.Red, InvertedColor.Green, InvertedColor.Blue, InvertedColor.OneTon, x == InvertedColor.Red);
}


fun main(c: Color) {
    __expect_type(c, "Color");
    __expect_type(C_BLUE, "Color");
    __expect_type(Color.createBlue(), "Color");
    __expect_type(ColorAlias.Red, "Color");
    __expect_type(EPartialAssigned.Max, "EPartialAssigned");
    __expect_type(Box{value:Color.Red}, "Box<Color>");
    return c;
}


/**
@testcase | 0   | 123 | 123
@testcase | 101 |     | -1 0 -1 -1 0 -1 -1 2
@testcase | 102 | 1   | -1 0 0 0 0 0
@testcase | 103 |     | -100 -99 1 2 3 4 5 6 2147483647
@testcase | 104 | -1  | -1 -1 2 -1 1
@testcase | 105 |     | 123 -10 456 2147483647 2147483648
@testcase | 106 | 1   | 1 1 1 1 100 -100
@testcase | 107 |     | 0 1 -100 2
@testcase | 108 |     | -1 0 -1 0 0 -1 0 typeid-1 -200
@testcase | 109 |     | 100 200 300 5
@testcase | 110 | 0   | [ 1 1 -100 88 -100 1 ] 
@testcase | 110 | 1   | [ -100 2 -100 88 2 2 ]  
@testcase | 110 | 2   | [ -100 3 -100 88 3 3 ]  
@testcase | 111 | 2   | 0 -1 -1 0  
@testcase | 112 | 0 1 | [ 210 120 ]
@testcase | 112 | 2 2 | [ 330 310 ]
@testcase | 113 | -8  | 256 -1 
@testcase | 113 | 111 | 257 0 
@testcase | 113 | 10  | 258 0 
@testcase | 113 | 50  | 0 0 
@testcase | 114 | 2   | 2 1 0 1000000000 -1 

@fif_codegen
"""
  test1() PROC:<{ 
    TRUE
    FALSE
    TRUE
    TRUE
    FALSE
    TRUE
    TRUE
    2 PUSHINT
  }>
"""

@fif_codegen
"""
  test2() PROC:<{               //  c
    DROP                        // 
    -1 PUSHINT                  //  '1=-1
    0 PUSHINT                   //  '1=-1 '2
    0 PUSHINT                   //  '1=-1 '2 '3=0
    s0 s0 s0 PUSH3              //  '1=-1 '2 '3=0 '4=0 '5=0 '6=0
  }>
"""

@fif_codegen DECLPROC checkIsColor<EPartialAssigned|builder>()

*/
