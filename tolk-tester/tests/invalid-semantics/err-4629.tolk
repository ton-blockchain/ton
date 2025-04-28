struct Wrapper<T = int> {
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
@stderr field `item` missed in initialization of struct `Wrapper<int>`
@stderr = Wrapper {
 */
