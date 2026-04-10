fun some<T, R>() {
    fun(c) {
        return c
    } as T -> R;
}

fun main() {
    some<int, slice>();
}

/**
@compilation_should_fail
@stderr can not convert type `int` to return type `slice` 
@stderr in lambda in function `some<int,slice>`
@stderr return c
 */
