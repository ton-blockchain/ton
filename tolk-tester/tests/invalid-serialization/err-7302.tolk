struct A{}
type StructAndNot = A | int8;

fun f() {
    var o = lazy StructAndNot.fromSlice("");   // neg5
    __expect_lazy("");
    match (o) {
        A => {}
        int8 => {}
    }
}

/**
@compilation_should_fail
@stderr `lazy` union should contain only structures, but it contains `int8`
 */
