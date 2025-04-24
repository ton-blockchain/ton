fun f(x: int) {}

fun cantAssignToVal() {
    val x = 10;
    f(x += 1);
}

/**
@compilation_should_fail
@stderr modifying immutable variable `x`
 */
