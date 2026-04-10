struct Wrapper<T> {
    item: T;
}

fun main() {
    var c = Wrapper<int> {  };
}

/**
@compilation_should_fail
@stderr field `item` missed in initialization of struct `Wrapper<int>`
 */
