type CustomInt = int;

fun CustomInt.packToBuilder(self, mutate b: builder) {
}

fun CustomInt.unpackFromSlice(self, mutate s: slice) {
}


fun main() {
    (5 as CustomInt).toCell()
}

/**
@compilation_should_fail
@stderr auto-serialization via toCell() is not available for type `CustomInt`
@stderr because `CustomInt.unpackFromSlice()` is declared incorrectly
@stderr hint: it must accept 1 parameter and return an object:
@stderr fun CustomInt.unpackFromSlice(mutate s: slice)
 */
