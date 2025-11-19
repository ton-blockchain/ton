struct User { id: int; }

fun absk(v: int) { return v > 0 ? v : -v; }

fun main() {
    absk(User);
}

/**
@compilation_should_fail
@stderr `User` only refers to a type, but is being used as a value here
@stderr absk(User)
*/
