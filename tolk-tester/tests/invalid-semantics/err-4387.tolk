struct Wrapper<T> {
    value: T;
}

type nested = Wrapper<Wrapper<Wrapper<int>>>;

fun main(d: nested) {
    var c1 = d;

    var c2 = Wrapper<Wrapper<Wrapper<int>>>;
}

/**
@compilation_should_fail
@stderr `Wrapper` only refers to a type, but is being used as a value here
@stderr c2 = Wrapper
 */
