struct Wrapper<T> {
    value: T;
}

fun my() {
    var w: Wrapper = Wrapper<int> { value: 30 };
}

/**
@compilation_should_fail
@stderr type `Wrapper<T>` is generic, you should provide type arguments
@stderr var w: Wrapper =
 */
