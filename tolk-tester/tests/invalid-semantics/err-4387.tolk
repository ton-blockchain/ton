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
@stderr struct `Wrapper<T>` can not be used as a value
@stderr c2 = Wrapper
 */
