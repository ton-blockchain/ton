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
@stderr field `item` missed in initialization of struct `Wrapper<T>`
@stderr = Wrapper {
 */
