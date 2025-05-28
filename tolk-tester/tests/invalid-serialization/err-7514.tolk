struct In4 {
    b: builder;
}

struct In3 {
    i: In4;
}

struct In2 {
    i: In3?;
}

struct In1 {
    i: (In2, slice);
}

struct MaybeNothing {}
struct MaybeJust<T> { value: T }
type Maybe<T> = MaybeNothing | MaybeJust<T>;

struct CantBe {
    a: address;
    b: address?;
    i: Maybe<In1>;
}

fun main(c: cell) {
    var d: CantBe = CantBe.fromCell(c);
}

/**
@compilation_should_fail
@stderr auto-serialization via fromCell() is not available for type `CantBe`
@stderr because field `CantBe.i` of type `Maybe<In1>` can't be serialized
@stderr because alias `Maybe<In1>` expands to `MaybeNothing | MaybeJust<In1>`
@stderr because variant #2 of type `MaybeJust<In1>` can't be serialized
@stderr because field `MaybeJust<In1>.value` of type `In1` can't be serialized
@stderr because field `In1.i` of type `(In2, slice)` can't be serialized
@stderr because element `tensor.0` of type `In2` can't be serialized
@stderr because field `In2.i` of type `In3?` can't be serialized
@stderr because field `In3.i` of type `In4` can't be serialized
@stderr because field `In4.b` of type `builder` can't be serialized
@stderr because type `builder` can not be used for reading, only for writing
@stderr hint: use `bitsN` or `RemainingBitsAndRefs` for reading
@stderr hint: using generics, you can substitute `builder` for writing and something other for reading
@stderr CantBe.fromCell(c)
 */
