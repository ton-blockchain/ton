
fun main() {
    lisp_list<int32> [1 as int, 3 as int32, 5 as int64];
}

/**
@compilation_should_fail
@stderr type `int64` is not assignable to `int32`
 */
