struct Bad {
    next: [Bad?]
}

struct WrappedBad {
    ref: Cell<Bad>
}

fun main(w: WrappedBad) {
    w.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `WrappedBad`
@stderr because field `WrappedBad.ref` of type `Cell<Bad>` can't be serialized
@stderr because field `Bad.next` of type `[Bad?]` can't be serialized
@stderr because element `shaped.0` of type `Bad?` can't be serialized
@stderr because variant #1 of type `Bad` can't be serialized
@stderr because struct `Bad` appears recursively in itself
 */
