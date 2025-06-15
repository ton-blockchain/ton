struct A {
}

fun neg4() {
    // because compiler doesn't analyze deep assignments
    var (a, b) = (1, lazy A.fromSlice(""));
}

/**
@compilation_should_fail
@stderr error: incorrect `lazy` operator usage, it's not directly assigned to a variable
 */
