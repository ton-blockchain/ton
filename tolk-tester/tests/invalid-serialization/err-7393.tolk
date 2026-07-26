struct Empty {}
type EmptyOrVoid = Empty | void

fun f(x: EmptyOrVoid) {
    x.toCell();
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `EmptyOrVoid`
@stderr because variant `Empty` can be indistinguishable from `void`
 */
