
global num: int;

@pure
fun set_pure() {
    (_, [num]) = (1, [2]);
}

/**
@compilation_should_fail
@stderr modifying a global in a pure function
@stderr ^^^
 */
