// try to cheat
fun moddiv2(x: int, y: int): (int, int) builtin;
// but actually, it remains unknown symbol

fun main() {
    moddiv2(1, 2);
}

/**
@compilation_should_fail
@stderr undefined symbol `moddiv2`
*/
