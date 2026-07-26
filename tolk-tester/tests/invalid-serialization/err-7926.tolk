struct Wrapped {
    ref: Cell<int>
}

fun main(w: Wrapped) {
    w.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `Wrapped`
@stderr because field `Wrapped.ref` of type `Cell<int>` can't be serialized
@stderr because type `int` is not serializable, it doesn't define binary width
 */
