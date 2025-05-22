struct Ok<T> { result: T; }

type OkInt = Ok<int>;
type OkSlice = Ok<slice>;

fun f(w: OkInt | int | OkSlice) {
    if (w !is Ok) {}
}

/**
@compilation_should_fail
@stderr can not deduce type arguments for `Ok<T>`, provide them manually
 */
