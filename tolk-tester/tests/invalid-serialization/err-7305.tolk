struct Counter7Increment { byValue: int7 }
struct Counter7Decrement { byValue: int7 }
type MsgEitherCounter = Counter7Increment | Counter7Decrement;

fun f() {
    // because .toCell()
    var msg = lazy MsgEitherCounter.fromSlice("");  // neg9
    match (msg) {
        Counter7Increment => {}
        Counter7Decrement => {}
    }
    return msg.toCell();
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `msg` is used in a non-lazy manner
@stderr lazy MsgEitherCounter
 */
