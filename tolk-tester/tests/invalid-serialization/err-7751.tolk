type CustomInt = int;

fun CustomInt.packToBuilder(self, b: builder) {
}

fun CustomInt.unpackFromSlice(mutate s: slice) {
}


fun main() {
    (5 as CustomInt).toCell()
}

/**
@compilation_should_fail
@stderr method `CustomInt.packToBuilder` is declared incorrectly
@stderr hint: it must accept 2 parameters and return nothing:
@stderr fun CustomInt.packToBuilder(self, mutate b: builder)
 */
