struct Wrapper<T> {
    field: T;
}

type MyInt = int;
type WrapperAlias<TTT> = Wrapper<TTT>;

fun main(w: int | WrapperAlias<int> | WrapperAlias<int8>) {
    match (w) {
        MyInt => {}
        Wrapper<MyInt> => {}
        WrapperAlias<int> => {}
    }
}

/**
@compilation_should_fail
@stderr wrong pattern matching: duplicated `Wrapper<int>`
@stderr WrapperAlias<int>
 */
