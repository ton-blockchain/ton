struct Counter7Increment { byValue: int7 }
struct Counter7Decrement { byValue: int7 }
type MsgEitherCounter = Counter7Increment | Counter7Decrement;

fun f(c: cell) {
    // because 2 matches
    var msg = lazy MsgEitherCounter.fromCell(c);   // neg8
    var t = createEmptyTuple();
    match (msg) {
        Counter7Increment => {}
        Counter7Decrement => {}
    }
    match (msg) {
        Counter7Increment => {}
        Counter7Decrement => {}
    }
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `msg` is used in a non-lazy manner
 */
