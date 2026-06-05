fun getTwo<T>(): T {
    return 2
}

fun demo() {
    var m: map<int32, int64> = [
        // <T> is inferred due to a hint, fun_ref is bound
        [getTwo(), getTwo()],
    ];
    // but later, an error is fired that only `[]` (empty map) is supported right now
}

/**
@compilation_should_fail
@stderr_avoid can not deduce T
@stderr `[...]` for `map` is not supported yet, only `[]` is allowed (empty map)
 */
