struct Inc { v: int32 }

type Msg = Inc

fun main(s: slice) {
    val msg = lazy Msg.fromSlice(s);
    match (msg) {
        else => throw 0xFFFF
    }
}

/**
@compilation_should_fail
@stderr `match` contains only `else`, but no variants
 */
