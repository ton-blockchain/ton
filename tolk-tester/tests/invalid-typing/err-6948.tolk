struct Wrapper<T> {
    field: T;
}

struct AnotherWrapper<T> {
    field: T;
}

fun main() {
    var w: Wrapper<int> = AnotherWrapper { field: 10 };
}

/**
@compilation_should_fail
@stderr can not assign `AnotherWrapper<int>` to variable of type `Wrapper<int>`
 */
