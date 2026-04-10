struct Wrapper<T> {
    value: T;
}

type nested = Wrapper<Wrapper<Wrapper<int>>>;

fun main() {
    var w3: nested = {};
}

/**
@compilation_should_fail
@stderr field `value` missed in initialization of struct `Wrapper<Wrapper<Wrapper<int>>>`
 */
