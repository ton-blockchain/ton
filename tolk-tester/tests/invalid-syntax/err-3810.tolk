
struct A {
    f: int;
}

fun main(a: A) {
    a.f--;
}

/**
@compilation_should_fail
@stderr Tolk has no decrement operator
@stderr hint: use `i -= 1`, not `i--`
 */
