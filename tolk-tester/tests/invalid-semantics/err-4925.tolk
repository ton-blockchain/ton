struct Wrapper<T> {
    field: T;
}

type MyInt = int;
type WrapperAlias<TTT> = Wrapper<TTT>;

fun main(w: int | WrapperAlias<int> | WrapperAlias<slice>) {
    match (w) {
        MyInt => {}
        WrapperAlias => {}
        WrapperAlias<slice> => {}
    }
}

/**
@compilation_should_fail
@stderr can not deduce type arguments for `WrapperAlias<TTT>`, provide them manually
@stderr WrapperAlias => {}
 */
