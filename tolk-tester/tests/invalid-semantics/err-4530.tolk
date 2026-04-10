struct Wrapper<T> {
    field: T;
}

fun main() {
    var w1: int | Wrapper<int> | null | Wrapper<int8> | (int, int) = Wrapper<int> { field: 10 };
    var w2: int | Wrapper<int> | null | Wrapper<int8> | (int, int) = Wrapper<int8> { field: 10 };
    var w3: int | Wrapper<int> | null | Wrapper<int8> | (int, int) = Wrapper { field: 10 };

    var w4: int | Wrapper<int> | null | Wrapper<int8> | (int, int) = { field: 20 };
}

/**
@compilation_should_fail
@stderr can not detect struct name
@stderr { field: 20 }
 */
