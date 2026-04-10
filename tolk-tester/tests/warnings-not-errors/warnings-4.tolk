struct Wrapper<T> {
    value: T;
}

fun main() {
    var w = Wrapper { value: 16 as int16 };
    return (w is Wrapper, w is Wrapper<int16>, w is Wrapper<int>, w !is Wrapper< Wrapper<int> >);
}

/**
@testcase | 0 |    | -1 -1 0 -1

@stderr warning: variable `w` of type `Wrapper<int16>` can never be `Wrapper<int>`, this condition is always false
@stderr warning: variable `w` of type `Wrapper<int16>` can never be `Wrapper<Wrapper<int>>`, this condition is always true
 */
