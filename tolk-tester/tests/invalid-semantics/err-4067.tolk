
global gShh: [slice, int];

@pure
fun increment(mutate i: int) {}

@pure
fun set_pure() {
    // ok
    var i = 0;
    var sh: [string, int] = ["", 0];
    increment(mutate i);
    increment(mutate sh.1);
    // err
    increment(mutate gShh.1);
}

/**
@compilation_should_fail
@stderr modifying a global in a pure function
@stderr gShh
 */
