type CustomInt = int;

fun CustomInt.packToBuilder(self, mutate b: builder) {
}

fun CustomInt.unpackFromSlice(mutate s: slice) {
    if (s.remainingBitsCount() > 10) {
        return 123;
    }
    return 600;
}

fun main() {
    var s = "";
    s.loadAny<CustomInt>();
}

/**
@compilation_should_fail
@stderr auto-serialization via loadAny() is not available for type `CustomInt`
@stderr because `CustomInt.unpackFromSlice()` can't be inlined; probably, it contains `return` in the middle
 */
