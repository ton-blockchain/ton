// @compilation_should_fail

type CustomInt1 = int;

// @stderr method `CustomInt1.packToBuilder` is declared incorrectly
// @stderr hint: it must accept 2 parameters and return nothing
// @stderr fun CustomInt1.packToBuilder(self, mutate b: builder)
fun CustomInt1.packToBuilder(self, b: builder) {
}

fun CustomInt1.unpackFromSlice(mutate s: slice): CustomInt1 {
    return 0;
}

fun errCustomInt1() {
    (5 as CustomInt1).toCell();
}

type CustomInt2 = int;

fun CustomInt2.packToBuilder(self, mutate b: builder) {
}

// @stderr method `CustomInt2.unpackFromSlice` is declared incorrectly
// @stderr hint: it must accept 1 parameter and return an object
// @stderr fun CustomInt2.unpackFromSlice(mutate s: slice)
fun CustomInt2.unpackFromSlice(self, mutate s: slice): CustomInt2 {
    return 0;
}

fun errCustomInt2() {
    (5 as CustomInt2).toCell();
}

type CustomInt3 = int;

fun CustomInt3.packToBuilder(self, mutate b: builder) {
}

// @stderr method `CustomInt3.unpackFromSlice` is declared incorrectly
fun CustomInt3.unpackFromSlice(mutate s: slice) {
    // wrong return type (tensor)
    return (1, 2, 3);
}

fun errCustomInt3() {
    (5 as CustomInt3).toCell();
}

