global g: int;

@pure
fun f_pure(): builder {
    var b: builder = beginCell();
    g = g + 1;
    return b;
}

fun main(): int {
    g = 0;
    f_pure();
    return g;
}

/**
@compilation_should_fail
@stderr an impure operation in a pure function
@stderr in function `f_pure`
@stderr g = g + 1;
 */
