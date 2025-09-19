
fun main() {
    var m: map<int8, int>;
}

/**
@compilation_should_fail
@stderr invalid `map`: type `int` can not be used as a value
@stderr because it can not be serialized
@stderr because type `int` is not serializable, it doesn't define binary width
@stderr hint: replace `int` with `int32` / `uint64` / `coins` / etc.
 */
