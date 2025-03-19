const op_increase = 0x123;

fun cantAssignToConst() {
    var x = 10;
    (x, op_increase) = (20, 30);
}

/**
@compilation_should_fail
@stderr modifying immutable constant
 */
