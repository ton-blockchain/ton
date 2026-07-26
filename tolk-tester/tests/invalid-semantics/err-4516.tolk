type MyInt = int

struct S {
    x: MyInt
}

fun MyInt.packToBuilder(self, mutate b: builder) {
    b.storeAny(S{x: self})
}

fun main() {
    return S { x: 1 }.toCell().hash();
}

/**
@compilation_should_fail
@stderr function `MyInt.packToBuilder` is recursive and leads to infinite serialization
 */
