struct Wrapper<T> {
    value: T;
}

type WrapperAlias<T> = Wrapper<T>;

fun main() {
    var w1 = WrapperAlias<int16> { value: 10 } as WrapperAlias<int8> | int | Wrapper<int16>;
    w1 is Wrapper;
}

/**
@compilation_should_fail
@stderr can not deduce type arguments for `Wrapper<T>`, provide them manually
@stderr w1 is Wrapper
 */
