struct A { id: int; value: int; }

fun takeTensor(v: (int, int)) {}

fun cantPassStructAsTensor() {
    takeTensor(A{id: 1, value:2});
}

/**
@compilation_should_fail
@stderr can not pass `A` to `(int, int)`
 */
