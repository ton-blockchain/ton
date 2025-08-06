struct Counter7Increment { byValue: int7 }
struct Counter7Decrement { byValue: int7 }
type MsgEitherCounter = Counter7Increment | Counter7Decrement;

fun f() {
    var msg = lazy MsgEitherCounter.fromSlice("");  // neg10
    match (msg) {
        Counter7Increment => 1 as int32,
        Counter7Decrement => 2 as int32,
        else => throw 123,      // not allowed in either
    }
}

/**
@compilation_should_fail
@stderr `else` is unreachable, because this `match` has only two options (0/1 prefixes)
 */
