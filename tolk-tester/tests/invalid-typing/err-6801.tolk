
fun main(a: slice | null | int8) {
    a as slice | null;
}

/**
@compilation_should_fail
@stderr type `slice | null | int8` can not be cast to `slice?`
 */
