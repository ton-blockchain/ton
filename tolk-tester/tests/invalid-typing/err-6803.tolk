fun takeAnySlice(s: slice) {}

fun cantPassBytesNToSlice(c: bits16) {
    takeAnySlice(c as slice);    // ok
    takeAnySlice(c);
}

/**
@compilation_should_fail
@stderr can not pass `bits16` to `slice`
@stderr takeAnySlice(c);
@stderr hint: use `as` operator for UNSAFE casting: `<some_expr> as slice`
 */
