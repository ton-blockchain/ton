fun cantAssignToVal() {
    val x = 10;
    var y = 20;
    [y, x] = [30, 40];
}

/**
@compilation_should_fail
@stderr modifying immutable variable `x`
 */
