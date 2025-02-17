struct User { id: int; }

fun absk(v: int) { return v > 0 ? v : -v; }

fun main() {
    absk(User);
}

/**
@compilation_should_fail
@stderr struct `User` can not be used as a value
@stderr absk(User)
*/
