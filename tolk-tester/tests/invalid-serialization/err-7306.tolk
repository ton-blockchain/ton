struct Counter7Increment { byValue: int7 }
struct Counter7Decrement { byValue: int7 }
type MsgEitherCounter = Counter7Increment | Counter7Decrement;

fun f() {
    // because in a complex expression
    var msg = lazy MsgEitherCounter.fromSlice("");
    match (msg) {
        Counter7Increment => 1 as int32,
        Counter7Decrement => 2 as int32,
    }.toCell()
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `msg` is used in a non-lazy manner
 */
