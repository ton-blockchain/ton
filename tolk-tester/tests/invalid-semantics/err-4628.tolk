struct Wrapper<T> {
    item: T;
    value: int;
}

fun main() {
    var c = Wrapper {
        value: null
    };
}

/**
@compilation_should_fail
@stderr can not deduce T for generic struct `Wrapper<T>`
@stderr = Wrapper {
 */
