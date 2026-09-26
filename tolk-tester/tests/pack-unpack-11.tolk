struct G<T> {
    x: int10
}

fun G<T>.packToBuilder<U = int>(self, mutate b: builder) {
    b.storeInt(self.x, 32);
}

fun G<T>.unpackFromSlice<U = int>(mutate s: slice): G<T> {
    return { x: s.loadInt(32) };
}

fun main() {
    var s = G<int32> { x: 1 }.toCell().beginParse();
    return (s.remainingBitsCount(), s.loadAny<G<int16>>(), s.remainingBitsCount());
}

/**
@testcase | 0 | | 32 1 0
*/
