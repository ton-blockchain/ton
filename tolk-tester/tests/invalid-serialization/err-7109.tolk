struct Recurrent {
    prefix: int32
    nextSh: [Recurrent?]
}

struct A {
    v: (int32, [Recurrent])
}

fun main(a: A) {
    return a.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `A`
@stderr because field `A.v` of type `(int32, [Recurrent])` can't be serialized
@stderr because element `tensor.1` of type `[Recurrent]` can't be serialized
@stderr because element `shaped.0` of type `Recurrent` can't be serialized
@stderr because field `Recurrent.nextSh` of type `[Recurrent?]` can't be serialized
@stderr because element `shaped.0` of type `Recurrent?` can't be serialized
@stderr because variant #1 of type `Recurrent` can't be serialized
@stderr because struct `Recurrent` appears recursively in itself
 */
