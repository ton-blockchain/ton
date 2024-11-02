fun cantRedefImmutable() {
    val x = 10;
    var (y: int, x redef) = (20, 30);
    return (y, x);
}

/**
@compilation_should_fail
@stderr modifying an immutable variable `x` (left side of assignment)
 */
