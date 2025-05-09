struct S { a: int; b: int; }

fun cantCastStructToTensor(s: S) {
    s as (int, int);
}

/**
@compilation_should_fail
@stderr type `S` can not be cast to `(int, int)`
 */
