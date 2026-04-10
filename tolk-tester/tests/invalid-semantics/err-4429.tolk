struct Wrapper<T> {
    field: T;
}

type MyInt = int;

fun main(w: int | Wrapper<int> | Wrapper<int8>) {
    w is Wrapper<int>;
    w is Wrapper<MyInt>;
    w is Wrapper<int8>;
    w is Wrapper<slice>;

    w is Wrapper;
}

/**
@compilation_should_fail
@stderr can not deduce type arguments for `Wrapper<T>`, provide them manually
@stderr Wrapper
 */
