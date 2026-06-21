struct LazyAssignTrio {
    a: uint8
    b: uint8
    c: uint8
}

fun main() {
    var st = lazy LazyAssignTrio.fromSlice("010203".hexToSlice());
    var other = lazy LazyAssignTrio.fromSlice("AABBCC".hexToSlice());
    st = other;
    return st.c;
}

/**
@compilation_should_fail
@stderr assigning one lazy variable to another is not supported
 */
