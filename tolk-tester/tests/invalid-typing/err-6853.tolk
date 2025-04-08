fun cantAssignIncompatiblePrimitiveNullable(m: int?) {
    var a: slice? = m;
}

/**
@compilation_should_fail
@stderr can not assign `int?` to variable of type `slice?`
 */
