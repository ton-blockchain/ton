struct (0x11) A {
    x: int8
}

fun main(s: slice): int {
    val a = lazy A.fromSlice(s);
    match (a) {
        A => { }
        else => { return 0 }
    }
    return a.x;
}

/**
@compilation_should_fail
@stderr `lazy` will not work here, because variable `a` is used both for lazy matching and in a non-lazy manner
 */
