// @compilation_should_fail

@pure
fun increment(mutate i: int) {}

// @stderr an impure operation in a pure function
// @stderr modifying a global `gTens` in a pure function
// @stderr in function `f_pure`
@pure
fun f_pure(): int {
    return f_impure();
    (gTens!).0 = 888;
}

// @stderr_avoid in function `f_impure`
fun f_impure(): int {
    (gTens!).0 = 888;
    return 0;
}

global gTens: (int, int);
global num: int;
global gShh: [slice, int];

// @stderr modifying a global `num` in a pure function
// @stderr modifying a global `gShh` in a pure function
// @stderr mutate gShh
// @stderr_avoid mutate i
// @stderr_avoid mutate sh
@pure
fun set_pure() {
    (_, [num]) = (1, [2]);
    // ok
    var i = 0;
    var sh: [string, int] = ["", 0];
    _ = increment(mutate i);
    _ = increment(mutate sh.1);
    // err
    _ = increment(mutate gShh.1);
}

