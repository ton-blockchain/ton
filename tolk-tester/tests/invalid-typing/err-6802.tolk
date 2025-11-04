
fun main() {
    var m = createEmptyMap<int8, int32>();
    m.set(5, "");
}

/**
@compilation_should_fail
@stderr can not pass `slice` to `int32`
 */
