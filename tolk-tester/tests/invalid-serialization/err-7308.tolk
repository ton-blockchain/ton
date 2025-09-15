struct (0x01) A { }
struct (0x02) B { }
struct (0x03) C { }
type MyInput = A | B | C;

fun f() {
    var msg = lazy MyInput.fromSlice("");
    match (msg) {
        A => {}
        B => {}
        else => throw 123,
    }
}

/**
@compilation_should_fail
@stderr `match` does not cover all possible types; missing types are: `C`
 */
