global gVar: int;

fun main() {
    var x = gVar<int>;
    return x;
}

/**
@compilation_should_fail
@stderr type arguments not expected here
 */
