fun tupleLast<T>(t: tuple): T
    asm "LAST";

fun getTupleLastGetter<X>(): tuple -> X {
    return tupleLast<X>;
}

fun failTypeMismatch() {
    var t = createEmptyTuple();
    var c: cell = getTupleLastGetter<int>()(t);
}

/**
@compilation_should_fail
@stderr can not assign `int` to variable of type `cell`
 */
