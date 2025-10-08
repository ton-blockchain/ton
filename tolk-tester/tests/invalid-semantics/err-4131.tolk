struct ImmSlice {
    readonly s: slice
}

fun ImmSlice.read(mutate self) {
    self.s.loadRef()
}

/**
@compilation_should_fail
@stderr modifying readonly field `ImmSlice.s`
 */
