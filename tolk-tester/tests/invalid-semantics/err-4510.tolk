global gVar: int;

fun main() {
    var x = gVar<int>;
    return x;
}

/**
@compilation_should_fail
@stderr generic T not expected here
 */
